/* Laplace: the machine's filesystem (#247). See include/memfs.h. */

#include "memfs.h"
#include "snapshot_store.h"   /* snapshot_crc32 */

#include <stddef.h>

void* kmalloc(size_t size);
void  kfree(void* p);

#define PAGE 4096ULL

static void zero(void* p, uint64_t n) {
    uint8_t* b = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

static void copy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static bool name_eq(const char* a, uint32_t alen, const char* b, uint32_t blen) {
    if (alen != blen) return false;
    for (uint32_t i = 0; i < alen; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static memfs_inode_t* node(memfs_t* fs, uint32_t ino) {
    return ino && ino < fs->ninodes && fs->inodes[ino].used ? &fs->inodes[ino] : 0;
}

static uint64_t now(memfs_t* fs) { return fs->env.now_ns ? fs->env.now_ns() : 0; }

static void before_change(memfs_t* fs) {
    if (fs->env.before_change) fs->env.before_change();
}

/* ---- Inodes ---- */

static bool ensure_inodes(memfs_t* fs, uint32_t need) {
    if (need <= fs->ninodes) return true;
    uint32_t cap = fs->ninodes ? fs->ninodes : 64;
    while (cap < need) cap *= 2;
    memfs_inode_t* n = (memfs_inode_t*)kmalloc((size_t)cap * sizeof(memfs_inode_t));
    if (!n) return false;
    zero(n, (uint64_t)cap * sizeof(memfs_inode_t));
    if (fs->inodes) {
        copy(n, fs->inodes, (uint64_t)fs->ninodes * sizeof(memfs_inode_t));
        kfree(fs->inodes);
    }
    fs->inodes = n;
    fs->ninodes = cap;
    return true;
}

/* The lowest free inode number at or above 2 (1 is the root). */
static uint32_t alloc_inode(memfs_t* fs, uint16_t type, uint16_t mode) {
    uint32_t ino = 2;
    while (ino < fs->ninodes && fs->inodes[ino].used) ino++;
    if (!ensure_inodes(fs, ino + 1)) return 0;
    memfs_inode_t* n = &fs->inodes[ino];
    zero(n, sizeof(*n));
    n->used = true;
    n->type = type;
    n->mode = mode & 07777;
    n->mtime_ns = n->ctime_ns = now(fs);
    fs->used++;
    return ino;
}

static void free_pages(memfs_t* fs, memfs_inode_t* n, uint32_t from) {
    for (uint32_t i = from; i < n->npages; i++) {
        if (n->pages[i]) {
            fs->env.page_free(n->pages[i] & ~(PAGE - 1));
            n->pages[i] = 0;
        }
    }
}

static void free_inode(memfs_t* fs, uint32_t ino) {
    memfs_inode_t* n = node(fs, ino);
    if (!n) return;
    before_change(fs);
    free_pages(fs, n, 0);
    if (n->pages) kfree(n->pages);
    for (uint32_t i = 0; i < n->nents; i++) kfree(n->ents[i].name);
    if (n->ents) kfree(n->ents);
    zero(n, sizeof(*n));
    fs->used--;
}

static void drop_link(memfs_t* fs, uint32_t ino) {
    memfs_inode_t* n = node(fs, ino);
    if (!n) return;
    if (n->nlink) n->nlink--;
    if (n->nlink == 0 && n->opens == 0) free_inode(fs, ino);
}

void memfs_open_ref(memfs_t* fs, uint32_t ino, int delta) {
    memfs_inode_t* n = node(fs, ino);
    if (!n) return;
    if (delta > 0) n->opens += (uint32_t)delta;
    else if (n->opens >= (uint32_t)-delta) n->opens -= (uint32_t)-delta;
    if (n->nlink == 0 && n->opens == 0) free_inode(fs, ino);
}

/* ---- Directories ---- */

static int dir_find(memfs_inode_t* d, const char* name, uint32_t len) {
    for (uint32_t i = 0; i < d->nents; i++) {
        if (name_eq(d->ents[i].name, d->ents[i].len, name, len)) return (int)i;
    }
    return -1;
}

static int dir_add(memfs_t* fs, uint32_t dir, const char* name, uint32_t len, uint32_t ino) {
    memfs_inode_t* d = node(fs, dir);
    if (d->nents == d->cap) {
        uint32_t cap = d->cap ? d->cap * 2 : 8;
        memfs_dirent_t* e = (memfs_dirent_t*)kmalloc((size_t)cap * sizeof(memfs_dirent_t));
        if (!e) return -MEMFS_ENOMEM;
        if (d->ents) {
            copy(e, d->ents, (uint64_t)d->nents * sizeof(memfs_dirent_t));
            kfree(d->ents);
        }
        d->ents = e;
        d->cap = cap;
    }
    char* s = (char*)kmalloc(len + 1);
    if (!s) return -MEMFS_ENOMEM;
    copy(s, name, len);
    s[len] = 0;
    memfs_dirent_t ent = { ino, (uint16_t)len, s };
    d->ents[d->nents++] = ent;
    d->mtime_ns = d->ctime_ns = now(fs);
    return 0;
}

static void dir_remove(memfs_t* fs, uint32_t dir, int idx) {
    memfs_inode_t* d = node(fs, dir);
    kfree(d->ents[idx].name);
    for (uint32_t i = (uint32_t)idx; i + 1 < d->nents; i++) d->ents[i] = d->ents[i + 1];
    d->nents--;
    d->mtime_ns = d->ctime_ns = now(fs);
}

/* ---- Paths ---- */

static int walk(memfs_t* fs, uint32_t cwd, const char* path, bool follow_last, uint32_t* out,
                int depth);

/* Resolve a symlink's target relative to its directory. */
static int follow_link(memfs_t* fs, uint32_t dir, uint32_t link, uint32_t* out, int depth) {
    if (depth > 40) return -MEMFS_ELOOP;
    char target[MEMFS_PATH_MAX];
    int n = memfs_readlink(fs, link, target, sizeof(target) - 1);
    if (n < 0) return n;
    target[n] = 0;
    return walk(fs, dir, target, true, out, depth + 1);
}

static int walk(memfs_t* fs, uint32_t cwd, const char* path, bool follow_last, uint32_t* out,
                int depth) {
    if (!path || !path[0]) return -MEMFS_ENOENT;
    uint32_t cur = path[0] == '/' ? MEMFS_ROOT : cwd;
    if (!node(fs, cur)) return -MEMFS_ENOENT;
    const char* p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* s = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - s);
        if (len > MEMFS_NAME_MAX) return -MEMFS_ENAMETOOLONG;
        memfs_inode_t* d = node(fs, cur);
        if (d->type != MEMFS_DIR) return -MEMFS_ENOTDIR;
        if (name_eq(s, len, ".", 1)) continue;
        if (name_eq(s, len, "..", 2)) {
            cur = d->parent ? d->parent : MEMFS_ROOT;
            continue;
        }
        int i = dir_find(d, s, len);
        if (i < 0) return -MEMFS_ENOENT;
        uint32_t next = d->ents[i].ino;
        const char* rest = p;
        while (*rest == '/') rest++;
        bool last = *rest == 0;
        bool trailing_slash = last && *p == '/';
        if (node(fs, next)->type == MEMFS_LNK && (!last || follow_last || trailing_slash)) {
            int rc = follow_link(fs, cur, next, &next, depth);
            if (rc < 0) return rc;
        }
        cur = next;
    }
    *out = cur;
    return 0;
}

int memfs_lookup(memfs_t* fs, uint32_t cwd, const char* path, bool follow, uint32_t* ino) {
    return walk(fs, cwd, path, follow, ino, 0);
}

int memfs_parent(memfs_t* fs, uint32_t cwd, const char* path, uint32_t* dir, const char** name,
                 uint32_t* namelen) {
    if (!path || !path[0]) return -MEMFS_ENOENT;
    /* The last component, ignoring trailing slashes. */
    const char* end = path;
    while (*end) end++;
    while (end > path + 1 && end[-1] == '/') end--;
    const char* s = end;
    while (s > path && s[-1] != '/') s--;
    uint32_t len = (uint32_t)(end - s);
    if (len == 0) return -MEMFS_EEXIST;             /* "/" */
    if (len > MEMFS_NAME_MAX) return -MEMFS_ENAMETOOLONG;
    if (name_eq(s, len, ".", 1) || name_eq(s, len, "..", 2)) return -MEMFS_EINVAL;
    uint32_t d = path[0] == '/' ? MEMFS_ROOT : cwd;
    if (s > path) {
        char buf[MEMFS_PATH_MAX];
        uint32_t n = (uint32_t)(s - path);
        if (n >= sizeof(buf)) return -MEMFS_ENAMETOOLONG;
        copy(buf, path, n);
        buf[n] = 0;
        int rc = walk(fs, cwd, buf, true, &d, 0);
        if (rc < 0) return rc;
    }
    if (!node(fs, d)) return -MEMFS_ENOENT;
    if (node(fs, d)->type != MEMFS_DIR) return -MEMFS_ENOTDIR;
    *dir = d;
    *name = s;
    *namelen = len;
    return 0;
}

/* ---- Data ---- */

static bool ensure_pages(memfs_inode_t* n, uint64_t count) {
    if (count <= n->npages) return true;
    if (count > (1u << 26)) return false;            /* 256 GiB */
    uint32_t cap = n->npages ? n->npages : 4;
    while (cap < count) cap *= 2;
    uint64_t* p = (uint64_t*)kmalloc((size_t)cap * sizeof(uint64_t));
    if (!p) return false;
    zero(p, (uint64_t)cap * sizeof(uint64_t));
    if (n->pages) {
        copy(p, n->pages, (uint64_t)n->npages * sizeof(uint64_t));
        kfree(n->pages);
    }
    n->pages = p;
    n->npages = cap;
    return true;
}

/* A seed file's first change: copy its bytes into pages. */
static int unseed(memfs_t* fs, memfs_inode_t* n) {
    if (!n->seeded) return 0;
    uint64_t pages = (n->size + PAGE - 1) / PAGE;
    if (!ensure_pages(n, pages)) return -MEMFS_ENOMEM;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t f = fs->env.page_alloc();
        if (!f) return -MEMFS_ENOSPC;
        uint64_t len = n->size - i * PAGE < PAGE ? n->size - i * PAGE : PAGE;
        copy(fs->env.page_ptr(f), fs->seed + n->seed_off + i * PAGE, len);
        n->pages[i] = f;
    }
    n->seeded = false;
    return 0;
}

int64_t memfs_read(memfs_t* fs, uint32_t ino, uint64_t off, void* buf, uint64_t len) {
    memfs_inode_t* n = node(fs, ino);
    if (!n) return -MEMFS_EBADF;
    if (n->type == MEMFS_DIR) return -MEMFS_EISDIR;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = n->size - off;
    if (n->seeded) {
        copy(buf, fs->seed + n->seed_off + off, len);
        return (int64_t)len;
    }
    uint8_t* out = (uint8_t*)buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t idx = pos / PAGE, in = pos % PAGE;
        uint64_t chunk = PAGE - in < len - done ? PAGE - in : len - done;
        uint64_t e = idx < n->npages ? n->pages[idx] : 0;
        if (e) copy(out + done, (uint8_t*)fs->env.page_ptr(e & ~(PAGE - 1)) + in, chunk);
        else zero(out + done, chunk);
        done += chunk;
    }
    return (int64_t)len;
}

int64_t memfs_write(memfs_t* fs, uint32_t ino, uint64_t off, const void* buf, uint64_t len) {
    memfs_inode_t* n = node(fs, ino);
    if (!n) return -MEMFS_EBADF;
    if (n->type != MEMFS_REG) return -MEMFS_EISDIR;
    if (len == 0) return 0;
    if (off + len < off || off + len > (1ULL << 38)) return -MEMFS_EFBIG;
    before_change(fs);
    int rc = unseed(fs, n);
    if (rc < 0) return rc;
    if (!ensure_pages(n, (off + len + PAGE - 1) / PAGE)) return -MEMFS_ENOMEM;
    const uint8_t* in = (const uint8_t*)buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t idx = pos / PAGE, at = pos % PAGE;
        uint64_t chunk = PAGE - at < len - done ? PAGE - at : len - done;
        if (!n->pages[idx]) {
            uint64_t f = fs->env.page_alloc();
            if (!f) break;
            n->pages[idx] = f;
        }
        n->pages[idx] &= ~MEMFS_PAGE_KNOWN;
        copy((uint8_t*)fs->env.page_ptr(n->pages[idx] & ~(PAGE - 1)) + at, in + done, chunk);
        done += chunk;
    }
    if (done == 0) return -MEMFS_ENOSPC;
    if (off + done > n->size) n->size = off + done;
    n->mtime_ns = n->ctime_ns = now(fs);
    return (int64_t)done;
}

int memfs_truncate(memfs_t* fs, uint32_t ino, uint64_t size) {
    memfs_inode_t* n = node(fs, ino);
    if (!n) return -MEMFS_EBADF;
    if (n->type == MEMFS_DIR) return -MEMFS_EISDIR;
    if (n->type != MEMFS_REG) return -MEMFS_EINVAL;
    if (size > (1ULL << 38)) return -MEMFS_EFBIG;
    if (size == n->size) return 0;
    before_change(fs);
    int rc = unseed(fs, n);
    if (rc < 0) return rc;
    if (size < n->size) {
        uint64_t keep = (size + PAGE - 1) / PAGE;
        free_pages(fs, n, (uint32_t)keep);
        /* The tail of a kept partial page must read as zeros if regrown. */
        if (size % PAGE && keep - 1 < n->npages && n->pages[keep - 1]) {
            uint8_t* p = (uint8_t*)fs->env.page_ptr(n->pages[keep - 1] & ~(PAGE - 1));
            zero(p + size % PAGE, PAGE - size % PAGE);
            n->pages[keep - 1] &= ~MEMFS_PAGE_KNOWN;
        }
    }
    n->size = size;
    n->mtime_ns = n->ctime_ns = now(fs);
    return 0;
}

int memfs_readlink(memfs_t* fs, uint32_t ino, char* buf, uint32_t cap) {
    memfs_inode_t* n = node(fs, ino);
    if (!n) return -MEMFS_ENOENT;
    if (n->type != MEMFS_LNK) return -MEMFS_EINVAL;
    uint64_t len = n->size < cap ? n->size : cap;
    n->type = MEMFS_REG;                         /* read it like a file */
    int64_t got = memfs_read(fs, ino, 0, buf, len);
    n->type = MEMFS_LNK;
    return (int)got;
}

int memfs_stat(memfs_t* fs, uint32_t ino, memfs_stat_t* st) {
    memfs_inode_t* n = node(fs, ino);
    if (!n) return -MEMFS_ENOENT;
    st->ino = ino;
    st->type = n->type;
    st->mode = n->mode;
    uint32_t subdirs = 0;
    for (uint32_t i = 0; i < n->nents; i++) subdirs += node(fs, n->ents[i].ino) &&
                                                       node(fs, n->ents[i].ino)->type == MEMFS_DIR;
    st->nlink = n->type == MEMFS_DIR ? 2 + subdirs : n->nlink;
    st->size = n->type == MEMFS_DIR ? PAGE : n->size;
    uint64_t blocks = 0;
    if (n->seeded) blocks = (n->size + 511) / 512;
    for (uint32_t i = 0; i < n->npages; i++) blocks += n->pages[i] ? 8 : 0;
    st->blocks = blocks;
    st->mtime_ns = n->mtime_ns;
    st->ctime_ns = n->ctime_ns;
    return 0;
}

int memfs_chmod(memfs_t* fs, uint32_t ino, uint16_t mode) {
    memfs_inode_t* n = node(fs, ino);
    if (!n) return -MEMFS_ENOENT;
    n->mode = mode & 07777;
    n->ctime_ns = now(fs);
    return 0;
}

int memfs_set_mtime(memfs_t* fs, uint32_t ino, uint64_t ns) {
    memfs_inode_t* n = node(fs, ino);
    if (!n) return -MEMFS_ENOENT;
    n->mtime_ns = ns;
    return 0;
}

const uint8_t* memfs_seed_bytes(memfs_t* fs, uint32_t ino, uint64_t* len) {
    memfs_inode_t* n = node(fs, ino);
    if (!n || n->type != MEMFS_REG || !n->seeded) return 0;
    *len = n->size;
    return fs->seed + n->seed_off;
}

/* ---- Namespace changes ---- */

int memfs_create(memfs_t* fs, uint32_t cwd, const char* path, uint16_t type, uint16_t mode,
                 const char* target, uint32_t* ino) {
    uint32_t dir, len;
    const char* name;
    int rc = memfs_parent(fs, cwd, path, &dir, &name, &len);
    if (rc < 0) return rc;
    if (dir_find(node(fs, dir), name, len) >= 0) return -MEMFS_EEXIST;
    uint32_t n = alloc_inode(fs, type, mode);
    if (!n) return -MEMFS_ENOMEM;
    memfs_inode_t* in = &fs->inodes[n];
    in->nlink = 1;
    if (type == MEMFS_DIR) in->parent = dir;
    if (type == MEMFS_LNK && target) {
        uint64_t tl = 0;
        while (target[tl]) tl++;
        in->type = MEMFS_REG;
        int64_t w = memfs_write(fs, n, 0, target, tl);
        in->type = MEMFS_LNK;
        if (w < 0) {
            free_inode(fs, n);
            return (int)w;
        }
    }
    rc = dir_add(fs, dir, name, len, n);
    if (rc < 0) {
        free_inode(fs, n);
        return rc;
    }
    if (ino) *ino = n;
    return 0;
}

int memfs_unlink(memfs_t* fs, uint32_t cwd, const char* path, bool dir) {
    uint32_t d, len;
    const char* name;
    int rc = memfs_parent(fs, cwd, path, &d, &name, &len);
    if (rc < 0) return rc;
    int i = dir_find(node(fs, d), name, len);
    if (i < 0) return -MEMFS_ENOENT;
    uint32_t ino = node(fs, d)->ents[i].ino;
    memfs_inode_t* n = node(fs, ino);
    if (dir) {
        if (n->type != MEMFS_DIR) return -MEMFS_ENOTDIR;
        if (n->nents) return -MEMFS_ENOTEMPTY;
    } else if (n->type == MEMFS_DIR) {
        return -MEMFS_EISDIR;
    }
    dir_remove(fs, d, i);
    if (n->type == MEMFS_DIR) n->nlink = 1;      /* a directory has one name */
    drop_link(fs, ino);
    return 0;
}

static bool is_ancestor(memfs_t* fs, uint32_t a, uint32_t d) {
    for (int guard = 0; d && guard < 4096; guard++) {
        if (d == a) return true;
        if (d == MEMFS_ROOT) return false;
        d = node(fs, d)->parent;
    }
    return false;
}

int memfs_rename(memfs_t* fs, uint32_t cwd, const char* from, const char* to) {
    uint32_t fd, td, fl, tl;
    const char *fname, *tname;
    int rc = memfs_parent(fs, cwd, from, &fd, &fname, &fl);
    if (rc < 0) return rc;
    rc = memfs_parent(fs, cwd, to, &td, &tname, &tl);
    if (rc < 0) return rc;
    int fi = dir_find(node(fs, fd), fname, fl);
    if (fi < 0) return -MEMFS_ENOENT;
    uint32_t ino = node(fs, fd)->ents[fi].ino;
    memfs_inode_t* n = node(fs, ino);
    if (n->type == MEMFS_DIR && is_ancestor(fs, ino, td)) return -MEMFS_EINVAL;
    int ti = dir_find(node(fs, td), tname, tl);
    if (ti >= 0) {
        uint32_t victim = node(fs, td)->ents[ti].ino;
        if (victim == ino) return 0;
        memfs_inode_t* v = node(fs, victim);
        if (v->type == MEMFS_DIR && n->type != MEMFS_DIR) return -MEMFS_EISDIR;
        if (v->type != MEMFS_DIR && n->type == MEMFS_DIR) return -MEMFS_ENOTDIR;
        if (v->type == MEMFS_DIR && v->nents) return -MEMFS_ENOTEMPTY;
        /* Replace in place: the entry keeps its position. */
        node(fs, td)->ents[ti].ino = ino;
        if (v->type == MEMFS_DIR) v->nlink = 1;
        drop_link(fs, victim);
        fi = dir_find(node(fs, fd), fname, fl);  /* indices may not move, but be sure */
        dir_remove(fs, fd, fi);
    } else {
        rc = dir_add(fs, td, tname, tl, ino);
        if (rc < 0) return rc;
        dir_remove(fs, fd, dir_find(node(fs, fd), fname, fl));
    }
    if (n->type == MEMFS_DIR) n->parent = td;
    n->ctime_ns = now(fs);
    return 0;
}

int memfs_link(memfs_t* fs, uint32_t cwd, const char* from, const char* to) {
    uint32_t ino;
    int rc = memfs_lookup(fs, cwd, from, false, &ino);
    if (rc < 0) return rc;
    if (node(fs, ino)->type == MEMFS_DIR) return -MEMFS_EINVAL;
    uint32_t d, len;
    const char* name;
    rc = memfs_parent(fs, cwd, to, &d, &name, &len);
    if (rc < 0) return rc;
    if (dir_find(node(fs, d), name, len) >= 0) return -MEMFS_EEXIST;
    rc = dir_add(fs, d, name, len, ino);
    if (rc < 0) return rc;
    node(fs, ino)->nlink++;
    return 0;
}

int memfs_dirent(memfs_t* fs, uint32_t dir, uint32_t index, uint32_t* ino, const char** name,
                 uint32_t* namelen) {
    memfs_inode_t* d = node(fs, dir);
    if (!d) return -MEMFS_ENOENT;
    if (d->type != MEMFS_DIR) return -MEMFS_ENOTDIR;
    if (index == 0) { *ino = dir; *name = "."; *namelen = 1; return 0; }
    if (index == 1) { *ino = d->parent ? d->parent : MEMFS_ROOT; *name = ".."; *namelen = 2; return 0; }
    if (index - 2 >= d->nents) return -MEMFS_ENOENT;
    *ino = d->ents[index - 2].ino;
    *name = d->ents[index - 2].name;
    *namelen = d->ents[index - 2].len;
    return 0;
}

int memfs_path(memfs_t* fs, uint32_t ino, char* buf, uint32_t cap) {
    if (!node(fs, ino) || node(fs, ino)->type != MEMFS_DIR) return -MEMFS_ENOENT;
    if (cap < 2) return -MEMFS_ENAMETOOLONG;
    /* Build backwards from the end of buf, then move to the front. */
    uint32_t pos = cap - 1;
    buf[pos] = 0;
    uint32_t cur = ino;
    for (int guard = 0; cur != MEMFS_ROOT && guard < 4096; guard++) {
        uint32_t parent = node(fs, cur)->parent;
        memfs_inode_t* p = node(fs, parent);
        if (!p) return -MEMFS_ENOENT;
        int i = -1;
        for (uint32_t k = 0; k < p->nents; k++) {
            if (p->ents[k].ino == cur) { i = (int)k; break; }
        }
        if (i < 0) return -MEMFS_ENOENT;          /* removed while it was the cwd */
        uint32_t len = p->ents[i].len;
        if (pos < len + 1) return -MEMFS_ENAMETOOLONG;
        pos -= len;
        copy(buf + pos, p->ents[i].name, len);
        buf[--pos] = '/';
        cur = parent;
    }
    if (pos == cap - 1) buf[--pos] = '/';
    uint32_t n = cap - 1 - pos;
    for (uint32_t k = 0; k <= n; k++) buf[k] = buf[pos + k];
    return (int)n;
}

/* ---- Seed image (cpio "newc") ---- */

static uint32_t hex8(const uint8_t* p, bool* ok) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else { *ok = false; return 0; }
        v = (v << 4) | d;
    }
    return v;
}

/* mkdir -p for a seed path's directories; returns the directory to hold the
 * last component. */
static int seed_dirs(memfs_t* fs, const char* path, uint32_t plen, uint32_t* dir,
                     const char** last, uint32_t* lastlen) {
    uint32_t cur = MEMFS_ROOT;
    uint32_t i = 0;
    for (;;) {
        while (i < plen && path[i] == '/') i++;
        uint32_t s = i;
        while (i < plen && path[i] != '/') i++;
        uint32_t len = i - s;
        while (i < plen && path[i] == '/') i++;
        if (i >= plen) {
            *dir = cur;
            *last = path + s;
            *lastlen = len;
            return 0;
        }
        if (len == 0 || name_eq(path + s, len, ".", 1)) continue;
        memfs_inode_t* d = node(fs, cur);
        int k = dir_find(d, path + s, len);
        if (k >= 0) {
            cur = d->ents[k].ino;
            if (node(fs, cur)->type != MEMFS_DIR) return -MEMFS_ENOTDIR;
            continue;
        }
        uint32_t n = alloc_inode(fs, MEMFS_DIR, 0755);
        if (!n) return -MEMFS_ENOMEM;
        fs->inodes[n].nlink = 1;
        fs->inodes[n].parent = cur;
        fs->inodes[n].mtime_ns = fs->inodes[n].ctime_ns = 0;
        if (dir_add(fs, cur, path + s, len, n) < 0) return -MEMFS_ENOMEM;
        cur = n;
    }
}

static int load_seed(memfs_t* fs) {
    const uint8_t* b = fs->seed;
    uint64_t off = 0;
    while (off + 110 <= fs->seed_len) {
        const uint8_t* h = b + off;
        if (h[0] != '0' || h[1] != '7' || h[2] != '0' || h[3] != '7' || h[4] != '0' ||
            (h[5] != '1' && h[5] != '2')) {
            return -MEMFS_EIO;
        }
        bool ok = true;
        uint32_t mode = hex8(h + 14, &ok);
        uint32_t mtime = hex8(h + 46, &ok);
        uint32_t fsize = hex8(h + 54, &ok);
        uint32_t nsize = hex8(h + 94, &ok);
        if (!ok || nsize == 0) return -MEMFS_EIO;
        uint64_t name_off = off + 110;
        uint64_t data_off = (name_off + nsize + 3) & ~3ULL;
        if (data_off + fsize > fs->seed_len) return -MEMFS_EIO;
        const char* name = (const char*)(b + name_off);
        uint32_t nlen = nsize - 1;
        if (name_eq(name, nlen, "TRAILER!!!", 10)) return 0;
        off = (data_off + fsize + 3) & ~3ULL;
        /* Strip "./" and "/" prefixes; skip the root itself. */
        while (nlen && (name[0] == '/' || (nlen >= 2 && name[0] == '.' && name[1] == '/'))) {
            if (name[0] == '/') { name++; nlen--; }
            else { name += 2; nlen -= 2; }
        }
        if (nlen == 0 || name_eq(name, nlen, ".", 1)) continue;
        uint32_t dir, lastlen;
        const char* last;
        int rc = seed_dirs(fs, name, nlen, &dir, &last, &lastlen);
        if (rc < 0) return rc;
        if (lastlen == 0 || lastlen > MEMFS_NAME_MAX) return -MEMFS_EIO;
        uint32_t kind = mode & 0170000;
        memfs_inode_t* d = node(fs, dir);
        int existing = dir_find(d, last, lastlen);
        if (kind == 0040000) {
            if (existing >= 0) {
                memfs_inode_t* e = node(fs, d->ents[existing].ino);
                if (e->type != MEMFS_DIR) return -MEMFS_EIO;
                e->mode = (uint16_t)(mode & 07777);
                e->mtime_ns = e->ctime_ns = (uint64_t)mtime * 1000000000ULL;
                continue;
            }
            uint32_t n = alloc_inode(fs, MEMFS_DIR, (uint16_t)mode);
            if (!n) return -MEMFS_ENOMEM;
            fs->inodes[n].nlink = 1;
            fs->inodes[n].parent = dir;
            fs->inodes[n].mtime_ns = fs->inodes[n].ctime_ns = (uint64_t)mtime * 1000000000ULL;
            if (dir_add(fs, dir, last, lastlen, n) < 0) return -MEMFS_ENOMEM;
        } else if (kind == 0100000 || kind == 0120000) {
            if (existing >= 0) return -MEMFS_EIO;    /* duplicate entry */
            uint32_t n = alloc_inode(fs, kind == 0100000 ? MEMFS_REG : MEMFS_LNK, (uint16_t)mode);
            if (!n) return -MEMFS_ENOMEM;
            memfs_inode_t* in = &fs->inodes[n];
            in->nlink = 1;
            in->size = fsize;
            in->seeded = true;
            in->seed_off = data_off;
            in->mtime_ns = in->ctime_ns = (uint64_t)mtime * 1000000000ULL;
            if (dir_add(fs, dir, last, lastlen, n) < 0) return -MEMFS_ENOMEM;
        }
        /* Devices, fifos, sockets: not in a seed. */
    }
    return -MEMFS_EIO;   /* no trailer */
}

int memfs_init(memfs_t* fs, const memfs_env_t* env, const uint8_t* seed, uint64_t len) {
    zero(fs, sizeof(*fs));
    fs->env = *env;
    fs->seed = seed;
    fs->seed_len = seed ? len : 0;
    fs->seed_crc = seed ? snapshot_crc32(0, seed, (uint32_t)len) : 0;
    if (!ensure_inodes(fs, 64)) return -MEMFS_ENOMEM;
    memfs_inode_t* root = &fs->inodes[MEMFS_ROOT];
    root->used = true;
    root->type = MEMFS_DIR;
    root->mode = 0755;
    root->nlink = 1;
    root->parent = MEMFS_ROOT;
    fs->used = 1;
    if (seed) {
        int rc = load_seed(fs);
        if (rc < 0) return rc;
    }
    uint32_t tmp;
    if (memfs_lookup(fs, MEMFS_ROOT, "/tmp", true, &tmp) < 0) {
        int rc = memfs_create(fs, MEMFS_ROOT, "/tmp", MEMFS_DIR, 01777, 0, &tmp);
        if (rc < 0) return rc;
        fs->inodes[tmp].mtime_ns = fs->inodes[tmp].ctime_ns = 0;
    }
    fs->inodes[MEMFS_ROOT].mtime_ns = fs->inodes[MEMFS_ROOT].ctime_ns = 0;
    return 0;
}

void memfs_destroy(memfs_t* fs) {
    for (uint32_t i = 1; i < fs->ninodes; i++) {
        if (fs->inodes[i].used) free_inode(fs, i);
    }
    if (fs->inodes) kfree(fs->inodes);
    fs->inodes = 0;
    fs->ninodes = 0;
    fs->used = 0;
}

/* ---- Keyframes ---- */

static void put(ksf_writer_t* w, const void* p, uint32_t n) { ksf_put(w, p, n); }

void memfs_save(memfs_t* fs, ksf_writer_t* w) {
    ksf_put_u32(w, fs->seed_crc);
    ksf_put_u64(w, fs->seed_len);
    ksf_put_u32(w, fs->used);
    for (uint32_t ino = 1; ino < fs->ninodes; ino++) {
        memfs_inode_t* n = &fs->inodes[ino];
        if (!n->used) continue;
        ksf_put_u32(w, ino);
        uint32_t tm = (uint32_t)n->type | ((uint32_t)n->mode << 16);
        ksf_put_u32(w, tm);
        ksf_put_u32(w, n->nlink);
        ksf_put_u32(w, n->opens);
        ksf_put_u32(w, n->parent);
        ksf_put_u32(w, n->seeded ? 1 : 0);
        ksf_put_u64(w, n->size);
        ksf_put_u64(w, n->mtime_ns);
        ksf_put_u64(w, n->ctime_ns);
        ksf_put_u64(w, n->seed_off);
        ksf_put_u32(w, n->nents);
        for (uint32_t i = 0; i < n->nents; i++) {
            ksf_put_u32(w, n->ents[i].ino);
            ksf_put_u32(w, n->ents[i].len);
            put(w, n->ents[i].name, n->ents[i].len);
        }
    }
}

int memfs_restore(memfs_t* fs, ksf_reader_t* r, uint32_t version) {
    if (version != 1) return -MEMFS_EINVAL;
    uint32_t crc = ksf_get_u32(r);
    uint64_t len = ksf_get_u64(r);
    if (!r->ok || crc != fs->seed_crc || len != fs->seed_len) return -MEMFS_EXDEV;
    /* Drop everything; pages that a restore keeps were taken out already. */
    memfs_env_t env = fs->env;
    const uint8_t* seed = fs->seed;
    memfs_destroy(fs);
    fs->env = env;
    fs->seed = seed;
    fs->seed_len = len;
    fs->seed_crc = crc;
    uint32_t count = ksf_get_u32(r);
    for (uint32_t k = 0; k < count && r->ok; k++) {
        uint32_t ino = ksf_get_u32(r);
        uint32_t tm = ksf_get_u32(r);
        if (!r->ok || ino == 0 || ino > (1u << 24) || !ensure_inodes(fs, ino + 1) || fs->inodes[ino].used) {
            return -MEMFS_EINVAL;
        }
        memfs_inode_t* n = &fs->inodes[ino];
        zero(n, sizeof(*n));
        n->used = true;
        fs->used++;
        n->type = (uint16_t)(tm & 0xFFFF);
        n->mode = (uint16_t)(tm >> 16);
        n->nlink = ksf_get_u32(r);
        n->opens = ksf_get_u32(r);
        n->parent = ksf_get_u32(r);
        n->seeded = ksf_get_u32(r) != 0;
        n->size = ksf_get_u64(r);
        n->mtime_ns = ksf_get_u64(r);
        n->ctime_ns = ksf_get_u64(r);
        n->seed_off = ksf_get_u64(r);
        if (n->type < MEMFS_REG || n->type > MEMFS_LNK) return -MEMFS_EINVAL;
        if (n->seeded && (n->seed_off > fs->seed_len || n->size > fs->seed_len - n->seed_off)) {
            return -MEMFS_EINVAL;
        }
        if (!n->seeded && n->type != MEMFS_DIR &&
            !ensure_pages(n, (n->size + PAGE - 1) / PAGE)) {
            return -MEMFS_ENOMEM;
        }
        uint64_t mtime = n->mtime_ns, ctime = n->ctime_ns;
        uint32_t nents = ksf_get_u32(r);
        for (uint32_t i = 0; i < nents && r->ok; i++) {
            uint32_t child = ksf_get_u32(r);
            uint32_t nl = ksf_get_u32(r);
            char name[MEMFS_NAME_MAX + 1];
            if (nl == 0 || nl > MEMFS_NAME_MAX || !ksf_get(r, name, nl)) return -MEMFS_EINVAL;
            if (dir_add(fs, ino, name, nl, child) < 0) return -MEMFS_ENOMEM;
        }
        n = &fs->inodes[ino];                 /* dir_add may not move it, but be sure */
        n->mtime_ns = mtime;                  /* dir_add touched them */
        n->ctime_ns = ctime;
    }
    return r->ok ? 0 : -MEMFS_EINVAL;
}

int memfs_attach_page(memfs_t* fs, uint32_t ino, uint32_t index, uint64_t frame) {
    memfs_inode_t* n = node(fs, ino);
    if (!n || n->type == MEMFS_DIR || n->seeded || index >= n->npages || n->pages[index]) {
        return -MEMFS_EINVAL;
    }
    n->pages[index] = frame;
    return 0;
}

void memfs_for_each_page(memfs_t* fs, memfs_page_fn fn, void* ctx) {
    for (uint32_t ino = 1; ino < fs->ninodes; ino++) {
        memfs_inode_t* n = &fs->inodes[ino];
        if (!n->used || n->type == MEMFS_DIR) continue;
        for (uint32_t i = 0; i < n->npages; i++) {
            if (n->pages[i]) fn(ctx, ino, i, &n->pages[i]);
        }
    }
}

uint32_t memfs_checksum(memfs_t* fs, memfs_crc_fn page_crc) {
    uint32_t crc = snapshot_crc32(0, &fs->seed_crc, sizeof(fs->seed_crc));
    for (uint32_t ino = 1; ino < fs->ninodes; ino++) {
        memfs_inode_t* n = &fs->inodes[ino];
        if (!n->used) continue;
        uint64_t meta[8] = { ino, n->type, n->mode, n->nlink, n->parent, n->size,
                             n->mtime_ns, n->seeded ? n->seed_off : ~0ULL };
        crc = snapshot_crc32(crc, meta, sizeof(meta));
        for (uint32_t i = 0; i < n->nents; i++) {
            crc = snapshot_crc32(crc, &n->ents[i].ino, sizeof(uint32_t));
            crc = snapshot_crc32(crc, n->ents[i].name, n->ents[i].len);
        }
        for (uint32_t i = 0; i < n->npages; i++) {
            if (!n->pages[i]) continue;
            uint32_t pc[2] = { i, page_crc(ino, i, n->pages[i]) };
            crc = snapshot_crc32(crc, pc, sizeof(pc));
        }
    }
    return crc;
}
