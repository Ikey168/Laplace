/* Host-side unit test for the kernel-state format (#243).
 *
 * Verifies:
 *   1. Several subsystems round-trip through one blob, in registration order,
 *      each with its own section version.
 *   2. Variable-size state: a process-like subsystem with any number of
 *      objects, each with any number of regions (no fixed caps), and a
 *      section larger than a page.
 *   3. A flipped byte anywhere in the blob is refused (CRC), as is a
 *      truncated blob.
 *   4. A section no subsystem claims, or a missing section, is refused; a
 *      failing restore names its subsystem.
 *   5. Measure and encode agree; a too-small buffer is refused.
 *   6. Cross-references are rebuilt by id, not by allocation order: a
 *      "files" subsystem restored after "procs" resolves its owners.
 *
 * Build: gcc -I../include -o test_kstate_format \
 *            test_kstate_format.c ../kernel/kstate_format.c ../kernel/snapshot_store.c
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
extern int printf(const char*, ...);

#include "kstate_format.h"

/* snapshot_store.c (snapshot_crc32) allocates elsewhere. */
void* kmalloc(size_t s) { return malloc(s); }
void  kfree(void* p) { free(p); }

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

/* ---- A process-like subsystem: N objects with M regions each ---- */

typedef struct { uint64_t start, end; uint32_t flags; } region_t;
typedef struct { uint32_t pid; uint32_t nreg; region_t reg[64]; } obj_t;
typedef struct { uint32_t n; obj_t objs[300]; } procs_t;

static void procs_save(ksf_writer_t* w, void* ctx) {
    procs_t* p = (procs_t*)ctx;
    ksf_put_u32(w, p->n);
    for (uint32_t i = 0; i < p->n; i++) {
        ksf_put_u32(w, p->objs[i].pid);
        ksf_put_u32(w, p->objs[i].nreg);
        for (uint32_t r = 0; r < p->objs[i].nreg; r++) ksf_put(w, &p->objs[i].reg[r], sizeof(region_t));
    }
}

static int procs_restore(ksf_reader_t* rd, uint32_t version, void* ctx) {
    procs_t* p = (procs_t*)ctx;
    if (version != 3) return -1;
    memset(p, 0, sizeof(*p));
    p->n = ksf_get_u32(rd);
    if (p->n > 300) return -1;
    for (uint32_t i = 0; i < p->n; i++) {
        p->objs[i].pid = ksf_get_u32(rd);
        p->objs[i].nreg = ksf_get_u32(rd);
        if (p->objs[i].nreg > 64) return -1;
        for (uint32_t r = 0; r < p->objs[i].nreg; r++) ksf_get(rd, &p->objs[i].reg[r], sizeof(region_t));
    }
    return rd->ok ? 0 : -1;
}

/* ---- A "files" subsystem whose entries name their owner by pid ---- */

typedef struct { uint32_t owner_pid; uint64_t offset; obj_t* owner; } file_t;
typedef struct { uint32_t n; file_t f[8]; procs_t* procs; } files_t;

static void files_save(ksf_writer_t* w, void* ctx) {
    files_t* f = (files_t*)ctx;
    ksf_put_u32(w, f->n);
    for (uint32_t i = 0; i < f->n; i++) {
        ksf_put_u32(w, f->f[i].owner_pid);
        ksf_put_u64(w, f->f[i].offset);
    }
}

static int files_restore(ksf_reader_t* rd, uint32_t version, void* ctx) {
    (void)version;
    files_t* f = (files_t*)ctx;
    f->n = ksf_get_u32(rd);
    if (f->n > 8) return -1;
    for (uint32_t i = 0; i < f->n; i++) {
        f->f[i].owner_pid = ksf_get_u32(rd);
        f->f[i].offset = ksf_get_u64(rd);
        f->f[i].owner = 0;
        for (uint32_t k = 0; k < f->procs->n; k++) {   /* resolve by id */
            if (f->procs->objs[k].pid == f->f[i].owner_pid) f->f[i].owner = &f->procs->objs[k];
        }
        if (!f->f[i].owner) return -1;
    }
    return rd->ok ? 0 : -1;
}

/* ---- A one-word subsystem whose restore can be told to fail ---- */

typedef struct { uint64_t v; bool fail; } word_t;
static void word_save(ksf_writer_t* w, void* ctx) { ksf_put_u64(w, ((word_t*)ctx)->v); }
static int word_restore(ksf_reader_t* rd, uint32_t version, void* ctx) {
    (void)version;
    word_t* w = (word_t*)ctx;
    w->v = ksf_get_u64(rd);
    return w->fail ? -1 : 0;
}
static uint32_t word_sum(void* ctx) { return (uint32_t)((word_t*)ctx)->v; }

static procs_t g_procs, g_procs2;
static files_t g_files, g_files2;
static word_t  g_word, g_word2;

static void setup(ksf_registry_t* reg, procs_t* p, files_t* f, word_t* w) {
    ksf_registry_init(reg);
    ksf_subsys_t sp = { 1, "procs", 3, procs_save, procs_restore, 0, p };
    ksf_subsys_t sf = { 2, "files", 1, files_save, files_restore, 0, f };
    ksf_subsys_t sw = { 7, "word", 9, word_save, word_restore, word_sum, w };
    ksf_register(reg, &sp);
    ksf_register(reg, &sf);
    ksf_register(reg, &sw);
    f->procs = p;
}

int main(void) {
    printf("=== Kernel-state format (#243) unit test ===\n");

    /* 300 objects with 0..63 regions each: far past any old fixed cap. */
    g_procs.n = 300;
    for (uint32_t i = 0; i < g_procs.n; i++) {
        g_procs.objs[i].pid = 1000 - i;            /* not allocation order */
        g_procs.objs[i].nreg = i % 64;
        for (uint32_t r = 0; r < g_procs.objs[i].nreg; r++) {
            g_procs.objs[i].reg[r].start = 0x10000ULL * (r + 1) + i;
            g_procs.objs[i].reg[r].end = 0x10000ULL * (r + 2) + i;
            g_procs.objs[i].reg[r].flags = r ^ i;
        }
    }
    g_files.n = 3;
    g_files.f[0] = (file_t){ 1000, 17, 0 };
    g_files.f[1] = (file_t){ 801, 4096, 0 };
    g_files.f[2] = (file_t){ 701, 1ULL << 40, 0 };
    g_word.v = 0xDEADBEEFCAFEULL;

    ksf_registry_t reg, reg2;
    setup(&reg, &g_procs, &g_files, &g_word);
    setup(&reg2, &g_procs2, &g_files2, &g_word2);

    /* --- 5. measure/encode --- */
    uint32_t size = ksf_measure(&reg);
    uint8_t* blob = malloc(size);
    CHECK(size > 4096 * 20, "a section can span many pages");
    CHECK(ksf_encode(&reg, blob, size - 1) == KSF_ERR_FULL, "a too-small buffer is refused");
    CHECK(ksf_encode(&reg, blob, size) == (int64_t)size, "encode writes exactly what measure said");
    CHECK(ksf_validate(blob, size) == KSF_OK, "the blob validates");

    /* --- 1, 2, 6. round trip --- */
    uint32_t failed = 0;
    CHECK(ksf_restore(&reg2, blob, size, &failed) == KSF_OK, "restore succeeds");
    CHECK(memcmp(&g_procs, &g_procs2, sizeof(g_procs)) == 0, "300 objects and their regions round-trip");
    CHECK(g_word2.v == g_word.v, "the one-word subsystem round-trips");
    CHECK(g_files2.n == 3 && g_files2.f[1].offset == 4096 && g_files2.f[2].offset == (1ULL << 40),
          "file offsets round-trip");
    CHECK(g_files2.f[1].owner == &g_procs2.objs[199] && g_files2.f[1].owner->pid == 801,
          "references are rebuilt by id into the restored objects");
    uint32_t ver = 0;
    ksf_reader_t rd;
    CHECK(ksf_section(blob, size, 7, &rd, &ver) && ver == 9 && rd.len == 8,
          "each section keeps its own version");
    CHECK(reg2.subs[2].checksum && reg2.subs[2].checksum(reg2.subs[2].ctx) == (uint32_t)g_word.v,
          "a subsystem's checksum callback sees its restored state");

    /* Determinism: the same state encodes to the same bytes. */
    uint8_t* blob2 = malloc(size);
    ksf_encode(&reg2, blob2, size);
    CHECK(memcmp(blob, blob2, size) == 0, "restored state re-encodes byte-identically");

    /* --- 3. corruption --- */
    int caught = 0, tried = 0;
    for (uint32_t off = 0; off < size; off += 997) {
        blob2[off] ^= 0x40;
        tried++;
        if (ksf_restore(&reg2, blob2, size, 0) != KSF_OK) caught++;
        blob2[off] ^= 0x40;
    }
    CHECK(caught == tried, "a flipped byte anywhere is refused");
    CHECK(ksf_restore(&reg2, blob, size - 1, 0) == KSF_ERR_CORRUPT, "a truncated blob is refused");

    /* --- 4. unknown / missing sections, failing restore --- */
    {
        ksf_registry_t small;
        ksf_registry_init(&small);
        ksf_subsys_t sp = { 1, "procs", 3, procs_save, procs_restore, 0, &g_procs2 };
        ksf_register(&small, &sp);
        failed = 0;
        CHECK(ksf_restore(&small, blob, size, &failed) == KSF_ERR_UNKNOWN && failed == 2,
              "a section no subsystem claims is refused, by id");

        ksf_registry_t big;
        procs_t* p3 = malloc(sizeof(procs_t));
        files_t f3;
        word_t w3 = { 0, false }, extra = { 0, false };
        setup(&big, p3, &f3, &w3);
        ksf_subsys_t sx = { 11, "extra", 1, word_save, word_restore, 0, &extra };
        CHECK(ksf_register(&big, &sx) == KSF_OK, "a fourth subsystem registers");
        CHECK(ksf_register(&big, &sx) == KSF_ERR_PARAM, "a duplicate id is refused");
        failed = 0;
        CHECK(ksf_restore(&big, blob, size, &failed) == KSF_ERR_MISSING && failed == 11,
              "a registered subsystem without a section is refused, by id");

        g_word2.fail = true;
        failed = 0;
        CHECK(ksf_restore(&reg2, blob, size, &failed) == KSF_ERR_RESTORE && failed == 7,
              "a failing restore names its subsystem");
        g_word2.fail = false;
        free(p3);
    }

    free(blob);
    free(blob2);
    if (failures == 0) {
        printf("PASSED: kernel state round-trips by subsystem, with no caps, and is checked\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
