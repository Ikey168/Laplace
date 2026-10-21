/* Host-side unit test for the machine's filesystem (#247).
 *
 * Verifies:
 *   1. A cpio seed loads: files read from the image, directories nest,
 *      symlinks resolve, and /tmp exists.
 *   2. Files: create, write past a hole, read back, truncate (the tail reads
 *      as zeros when regrown), and a seed file copied to pages at its first
 *      change while the image stays untouched.
 *   3. The namespace: mkdir, unlink, rmdir of a non-empty directory refused,
 *      rename over a file, rename of a directory into itself refused, hard
 *      links, an unlinked file living while open, getcwd paths.
 *   4. Keyframes: save then restore rebuilds the same tree (same checksum),
 *      pages re-attached by (inode, index); a different seed is refused.
 *   5. Every change to a page is announced first (before_change), and writes
 *      clear the page's KNOWN bit.
 *
 * Build: gcc -I../include -o test_memfs \
 *            test_memfs.c ../kernel/memfs.c ../kernel/kstate_format.c ../kernel/snapshot_store.c
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "memfs.h"

void* kmalloc(size_t s) { return malloc(s); }
void  kfree(void* p) { free(p); }

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

/* Frames are malloc'd, 4096-aligned blocks; the "frame" is the pointer. */
static int g_live_pages, g_changes;
static uint64_t page_alloc(void) {
    void* p = aligned_alloc(4096, 4096);
    memset(p, 0, 4096);
    g_live_pages++;
    return (uint64_t)(uintptr_t)p;
}
static void page_free(uint64_t f) { free((void*)(uintptr_t)f); g_live_pages--; }
static void* page_ptr(uint64_t f) { return (void*)(uintptr_t)f; }
static void before_change(void) { g_changes++; }
static uint64_t g_now = 1000;
static uint64_t now_ns(void) { return g_now; }

static const memfs_env_t ENV = { page_alloc, page_free, page_ptr, before_change, now_ns };

/* ---- a tiny cpio writer ---- */
static uint8_t g_cpio[8192];
static size_t g_len;
static void put_entry(uint32_t ino, const char* name, uint32_t mode, const char* data) {
    size_t nlen = strlen(name) + 1, dlen = data ? strlen(data) : 0;
    g_len += (size_t)sprintf((char*)g_cpio + g_len,
                             "070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X",
                             ino, mode, 0, 0, 1, 7, (unsigned)dlen, 0, 0, 0, 0, (unsigned)nlen, 0);
    memcpy(g_cpio + g_len, name, nlen);
    g_len += nlen;
    while (g_len % 4) g_cpio[g_len++] = 0;
    if (dlen) memcpy(g_cpio + g_len, data, dlen);
    g_len += dlen;
    while (g_len % 4) g_cpio[g_len++] = 0;
}

static bool contains(const uint8_t* hay, size_t n, const char* needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++) {
        if (memcmp(hay + i, needle, k) == 0) return true;
    }
    return false;
}

static uint32_t crc_of_page(uint32_t ino, uint32_t index, uint64_t entry) {
    (void)ino; (void)index;
    const uint8_t* p = page_ptr(entry & ~4095ULL);
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4096; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

typedef struct { uint64_t frames[64]; uint32_t ino[64], idx[64]; int n; } pages_t;
static void collect(void* c, uint32_t ino, uint32_t index, uint64_t* entry) {
    pages_t* p = (pages_t*)c;
    p->ino[p->n] = ino; p->idx[p->n] = index; p->frames[p->n] = *entry; p->n++;
    *entry = 0;   /* taken, as a restore harvest does */
}

int main(void) {
    printf("=== Filesystem (#247) unit test ===\n");
    put_entry(1, "etc", 0040755, 0);
    put_entry(2, "etc/motd", 0100644, "hello from the seed\n");
    put_entry(3, "scripts/run.lua", 0100755, "print('hi')\n");   /* implicit parent */
    put_entry(4, "bin", 0040755, 0);
    put_entry(5, "bin/motd-link", 0120777, "../etc/motd");
    put_entry(0, "TRAILER!!!", 0, 0);

    memfs_t fs;
    CHECK(memfs_init(&fs, &ENV, g_cpio, g_len) == 0, "the seed loads");
    uint32_t ino, ino2;
    char buf[8192];

    /* --- 1. seed --- */
    CHECK(memfs_lookup(&fs, MEMFS_ROOT, "/etc/motd", true, &ino) == 0 &&
          memfs_read(&fs, ino, 0, buf, sizeof(buf)) == 20 && memcmp(buf, "hello from the seed\n", 20) == 0,
          "a seed file reads from the image");
    CHECK(memfs_lookup(&fs, MEMFS_ROOT, "/scripts/run.lua", true, &ino2) == 0, "implicit parents are created");
    CHECK(memfs_lookup(&fs, MEMFS_ROOT, "/bin/motd-link", true, &ino2) == 0 && ino2 == ino,
          "a symlink resolves to its target");
    CHECK(memfs_lookup(&fs, MEMFS_ROOT, "/bin/motd-link", false, &ino2) == 0 && ino2 != ino &&
          memfs_readlink(&fs, ino2, buf, sizeof(buf)) == 11, "lstat sees the link itself");
    CHECK(memfs_lookup(&fs, MEMFS_ROOT, "/tmp", true, &ino2) == 0, "/tmp exists");
    CHECK(memfs_lookup(&fs, MEMFS_ROOT, "/etc/motd/x", true, &ino2) == -MEMFS_ENOTDIR,
          "a file is not a directory");

    /* --- 2. files --- */
    uint32_t f;
    CHECK(memfs_create(&fs, MEMFS_ROOT, "/tmp/out.txt", MEMFS_REG, 0644, 0, &f) == 0, "create a file");
    CHECK(memfs_create(&fs, MEMFS_ROOT, "/tmp/out.txt", MEMFS_REG, 0644, 0, 0) == -MEMFS_EEXIST,
          "... once");
    g_changes = 0;
    CHECK(memfs_write(&fs, f, 10000, "tail", 4) == 4 && g_changes > 0, "write past a hole, announced");
    memfs_stat_t st;
    memfs_stat(&fs, f, &st);
    CHECK(st.size == 10004 && st.blocks == 8, "the hole takes no page");
    memset(buf, 1, sizeof(buf));
    CHECK(memfs_read(&fs, f, 0, buf, 8192) == 8192 && buf[0] == 0 && buf[4095] == 0 &&
          buf[8191] == 0, "the hole reads as zeros");
    CHECK(memfs_read(&fs, f, 10000, buf, 100) == 4 && memcmp(buf, "tail", 4) == 0, "data reads back");
    memfs_write(&fs, f, 0, "abcdefgh", 8);
    CHECK(memfs_truncate(&fs, f, 4) == 0 && memfs_truncate(&fs, f, 8) == 0 &&
          memfs_read(&fs, f, 0, buf, 8) == 8 && memcmp(buf, "abcd\0\0\0\0", 8) == 0,
          "truncate, regrow: the tail reads as zeros");
    CHECK(memfs_write(&fs, ino, 6, "FROM", 4) == 4 && memfs_read(&fs, ino, 0, buf, 20) == 20 &&
          memcmp(buf, "hello FROM the seed\n", 20) == 0 &&
          contains(g_cpio, g_len, "hello from the seed"),
          "a seed file changes in pages; the image does not");

    /* --- 3. namespace --- */
    uint32_t d;
    CHECK(memfs_create(&fs, MEMFS_ROOT, "/tmp/d", MEMFS_DIR, 0755, 0, &d) == 0, "mkdir");
    CHECK(memfs_create(&fs, d, "inner", MEMFS_REG, 0644, 0, &ino2) == 0, "create relative to a directory");
    CHECK(memfs_unlink(&fs, MEMFS_ROOT, "/tmp/d", true) == -MEMFS_ENOTEMPTY, "rmdir of a full directory fails");
    CHECK(memfs_rename(&fs, MEMFS_ROOT, "/tmp/d", "/tmp/d/sub") == -MEMFS_EINVAL,
          "a directory cannot move into itself");
    CHECK(memfs_path(&fs, d, buf, sizeof(buf)) == 6 && strcmp(buf, "/tmp/d") == 0, "getcwd path");
    CHECK(memfs_rename(&fs, MEMFS_ROOT, "/tmp/d/inner", "/tmp/out.txt") == 0 &&
          memfs_lookup(&fs, MEMFS_ROOT, "/tmp/out.txt", true, &ino2) == 0 && ino2 != f,
          "rename over a file replaces it");
    CHECK(memfs_link(&fs, MEMFS_ROOT, "/tmp/out.txt", "/tmp/hard") == 0, "hard link");
    uint32_t keep;
    memfs_create(&fs, MEMFS_ROOT, "/tmp/open.txt", MEMFS_REG, 0600, 0, &keep);
    memfs_write(&fs, keep, 0, "still here", 10);
    memfs_open_ref(&fs, keep, 1);
    CHECK(memfs_unlink(&fs, MEMFS_ROOT, "/tmp/open.txt", false) == 0 &&
          memfs_lookup(&fs, MEMFS_ROOT, "/tmp/open.txt", true, &ino2) == -MEMFS_ENOENT &&
          memfs_read(&fs, keep, 0, buf, 10) == 10, "an unlinked file lives while open");

    /* --- 4/5. keyframes --- */
    pages_t taken = { .n = 0 };
    int pages_before = g_live_pages;
    uint32_t sum = memfs_checksum(&fs, crc_of_page);
    ksf_writer_t w = { 0, 0, 0 };
    memfs_save(&fs, &w);
    uint8_t* blob = malloc(w.len);
    ksf_writer_t w2 = { blob, w.len, 0 };
    memfs_save(&fs, &w2);
    memfs_for_each_page(&fs, collect, &taken);          /* harvest every page */
    g_now = 99999;                                        /* times must come from the blob */
    ksf_reader_t r = { blob, w.len, 0, true };
    CHECK(memfs_restore(&fs, &r, 1) == 0 && r.pos == r.len, "the metadata restores");
    for (int i = 0; i < taken.n; i++) memfs_attach_page(&fs, taken.ino[i], taken.idx[i], taken.frames[i]);
    CHECK(memfs_checksum(&fs, crc_of_page) == sum, "the restored tree has the same checksum");
    CHECK(g_live_pages == pages_before, "pages were moved, not copied or leaked");
    CHECK(memfs_lookup(&fs, MEMFS_ROOT, "/tmp/hard", true, &ino2) == 0 &&
          memfs_read(&fs, ino2, 0, buf, 1) >= 0 && memfs_read(&fs, keep, 0, buf, 10) == 10 &&
          memcmp(buf, "still here", 10) == 0, "files and the open orphan come back");
    memfs_lookup(&fs, MEMFS_ROOT, "/tmp/out.txt", true, &ino2);
    uint64_t* e = &fs.inodes[f].pages[0];
    (void)e;
    /* KNOWN bits: mark every page known, then a write clears exactly one. */
    for (uint32_t i = 1; i < fs.ninodes; i++)
        for (uint32_t k = 0; fs.inodes[i].used && k < fs.inodes[i].npages; k++)
            if (fs.inodes[i].pages[k]) fs.inodes[i].pages[k] |= MEMFS_PAGE_KNOWN;
    memfs_write(&fs, keep, 3, "L", 1);
    CHECK(!(fs.inodes[keep].pages[0] & MEMFS_PAGE_KNOWN), "a write clears the page's KNOWN bit");
    CHECK(memfs_checksum(&fs, crc_of_page) != sum, "and the checksum sees the change");

    /* A different seed is refused. */
    memfs_t other;
    g_cpio[110 + 12] ^= 1;
    memfs_init(&other, &ENV, g_cpio, g_len);
    ksf_reader_t r2 = { blob, w.len, 0, true };
    CHECK(memfs_restore(&other, &r2, 1) == -MEMFS_EXDEV, "a recording with another seed is refused");
    memfs_destroy(&other);

    memfs_open_ref(&fs, keep, -1);
    memfs_destroy(&fs);
    CHECK(g_live_pages == 0, "destroy frees every page");
    free(blob);
    if (failures == 0) {
        printf("PASSED: the filesystem behaves, and round-trips through a keyframe\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
