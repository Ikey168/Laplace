/* Laplace core: files and file descriptors for Linux programs (#247).
 * See include/core/files.h. */

#include "core/files.h"
#include "core/linux.h"
#include "core/kstate.h"
#include "core/kfpages.h"
#include "core/machine.h"
#include "core/timetravel.h"
#include "core/clock.h"
#include "core/mm.h"
#include "core/vm.h"
#include "core/console.h"
#include "memfs.h"
#include "entropy_record.h"
#include "divergence.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

static inline int64_t err(int e) { return -(int64_t)e; }

/* errno values beyond include/core/linux.h. */
#define E_EXDEV        18
#define E_ENOSPC       28
#define E_EROFS        30
#define E_ENAMETOOLONG 36
#define E_ELOOP        40
#define E_EOPNOTSUPP   95

/* open(2) flags. */
#define O_ACCMODE    03
#define O_WRONLY     01
#define O_RDWR       02
#define O_CREAT      0100
#define O_EXCL       0200
#define O_TRUNC      01000
#define O_APPEND     02000
#define O_NONBLOCK   04000
#define O_DIRECTORY  0200000
#define O_NOFOLLOW   0400000
#define O_CLOEXEC    02000000
#define O_PATH       010000000
#define O_TMPFILE    (020000000 | O_DIRECTORY)

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EMPTY_PATH       0x1000

#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFLNK  0120000

#define MAX_FDS  1024

/* ---- Open file descriptions ---- */

enum { OF_FREE = 0, OF_CONSOLE, OF_FILE, OF_DIR, OF_NULL, OF_ZERO, OF_RANDOM };

typedef struct {
    uint32_t refs;
    uint32_t kind;
    uint32_t ino;
    uint32_t flags;          /* access mode and status flags */
    uint64_t off;
} ofile_t;

typedef struct {
    uint32_t cwd;
    uint32_t umask;
    uint32_t nfd;
    int32_t  of[MAX_FDS];    /* description id, or -1 */
    uint8_t  cloexec[MAX_FDS];
} fdt_t;

static memfs_t   g_fs;
static ofile_t*  g_of;
static uint32_t  g_nof;
static uint64_t  g_fs_now;           /* file times: the last wall time read */
static lp_image_t g_seed_images[8];  /* programs loaded from the seed */
static char      g_seed_paths[8][64];
static uint32_t  g_nseed_images;

static fdt_t* fdt(process_t* p) { return (fdt_t*)p->files; }

static int32_t of_alloc(uint32_t kind, uint32_t ino, uint32_t flags) {
    uint32_t id = 0;
    while (id < g_nof && g_of[id].refs) id++;
    if (id == g_nof) {
        uint32_t cap = g_nof ? g_nof * 2 : 64;
        ofile_t* n = (ofile_t*)kzalloc((uint64_t)cap * sizeof(ofile_t));
        if (!n) return -1;
        if (g_of) {
            memcpy(n, g_of, (uint64_t)g_nof * sizeof(ofile_t));
            kfree(g_of);
        }
        g_of = n;
        g_nof = cap;
    }
    ofile_t* o = &g_of[id];
    o->refs = 1;
    o->kind = kind;
    o->ino = ino;
    o->flags = flags;
    o->off = 0;
    if (kind == OF_FILE || kind == OF_DIR) memfs_open_ref(&g_fs, ino, 1);
    return (int32_t)id;
}

static void of_put(int32_t id) {
    if (id < 0 || (uint32_t)id >= g_nof || !g_of[id].refs) return;
    ofile_t* o = &g_of[id];
    if (--o->refs) return;
    if (o->kind == OF_FILE || o->kind == OF_DIR) memfs_open_ref(&g_fs, o->ino, -1);
    memset(o, 0, sizeof(*o));
}

static ofile_t* of_get(process_t* p, int64_t fd) {
    fdt_t* t = fdt(p);
    if (!t || fd < 0 || fd >= MAX_FDS || t->of[fd] < 0) return 0;
    return &g_of[t->of[fd]];
}

static int64_t fd_install(process_t* p, int32_t of, int64_t lowest, bool cloexec) {
    fdt_t* t = fdt(p);
    for (int64_t fd = lowest < 0 ? 0 : lowest; fd < MAX_FDS; fd++) {
        if (t->of[fd] < 0) {
            t->of[fd] = of;
            t->cloexec[fd] = cloexec;
            return fd;
        }
    }
    return err(LX_EMFILE);
}

static void fd_close(process_t* p, int64_t fd) {
    fdt_t* t = fdt(p);
    of_put(t->of[fd]);
    t->of[fd] = -1;
    t->cloexec[fd] = 0;
}

/* ---- Process lifecycle ---- */

static fdt_t* fdt_new(void) {
    fdt_t* t = (fdt_t*)kzalloc(sizeof(fdt_t));
    if (!t) return 0;
    for (uint32_t i = 0; i < MAX_FDS; i++) t->of[i] = -1;
    t->cwd = MEMFS_ROOT;
    t->umask = 022;
    return t;
}

void files_proc_new(process_t* p) {
    fdt_t* t = fdt_new();
    if (!t) panic("files: out of memory for a descriptor table");
    p->files = t;
    int32_t con = of_alloc(OF_CONSOLE, 0, O_RDWR);
    t->of[0] = con;
    t->of[1] = con;
    t->of[2] = con;
    g_of[con].refs = 3;
}

void files_proc_gone(process_t* p) {
    fdt_t* t = fdt(p);
    if (!t) return;
    for (int64_t fd = 0; fd < MAX_FDS; fd++) {
        if (t->of[fd] >= 0) fd_close(p, fd);
    }
    kfree(t);
    p->files = 0;
}

/* ---- Paths ---- */

static int64_t mfs_err(int rc) { return (int64_t)rc; }   /* memfs errnos are Linux's */

/* A NUL-terminated path from user memory. */
static int64_t get_path(process_t* p, uint64_t upath, char* out, uint32_t cap) {
    if (!upath) return err(LX_EFAULT);
    for (uint32_t i = 0; i < cap; i++) {
        if (!uaccess_read(p->address_space, upath + i, &out[i], 1)) return err(LX_EFAULT);
        if (!out[i]) return i == 0 ? err(LX_ENOENT) : 0;
    }
    return err(E_ENAMETOOLONG);
}

/* The directory a *at() call's path is relative to. */
static int64_t base_dir(process_t* p, int64_t dirfd, const char* path, uint32_t* dir) {
    if (path[0] == '/' || (int32_t)dirfd == AT_FDCWD) {
        *dir = fdt(p)->cwd;
        return 0;
    }
    ofile_t* o = of_get(p, (int32_t)dirfd);
    if (!o) return err(LX_EBADF);
    if (o->kind != OF_DIR) return err(LX_ENOTDIR);
    *dir = o->ino;
    return 0;
}

static bool str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint32_t dev_kind(const char* path) {
    if (str_eq(path, "/dev/null")) return OF_NULL;
    if (str_eq(path, "/dev/zero")) return OF_ZERO;
    if (str_eq(path, "/dev/urandom") || str_eq(path, "/dev/random")) return OF_RANDOM;
    if (str_eq(path, "/dev/tty") || str_eq(path, "/dev/console")) return OF_CONSOLE;
    return 0;
}

/* ---- Reading and writing ---- */

static int64_t write_console(process_t* p, uint64_t buf, uint64_t len) {
    char tmp[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
        if (!uaccess_read(p->address_space, buf + done, tmp, n)) {
            return done ? (int64_t)done : err(LX_EFAULT);
        }
        machine_device_write(MACHINE_DEV_CONSOLE, tmp, n);
        done += n;
    }
    return (int64_t)done;
}

/* Read `len` bytes at `off` (or the description's offset if off < 0). */
static int64_t do_read(process_t* p, ofile_t* o, uint64_t buf, uint64_t len, int64_t off) {
    if ((o->flags & O_ACCMODE) == O_WRONLY) return err(LX_EBADF);
    uint8_t tmp[512];
    switch (o->kind) {
    case OF_CONSOLE:
    case OF_NULL:
        return 0;                                 /* console input: #249 */
    case OF_ZERO: {
        uint64_t done = 0;
        memset(tmp, 0, sizeof(tmp));
        while (done < len) {
            uint64_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
            if (!uaccess_write(p->address_space, buf + done, tmp, n)) return done ? (int64_t)done : err(LX_EFAULT);
            done += n;
        }
        return (int64_t)done;
    }
    case OF_RANDOM: {
        /* Journaled entropy (#242): one input record per read, up to 64 KiB. */
        static uint8_t rnd[65536];
        if (len > sizeof(rnd)) len = sizeof(rnd);
        if (kentropy_fill(rnd, (uint32_t)len) != ENTROPY_REC_OK) return err(LX_EAGAIN);
        return uaccess_write(p->address_space, buf, rnd, len) ? (int64_t)len : err(LX_EFAULT);
    }
    case OF_DIR:
        return err(LX_EISDIR);
    case OF_FILE:
        break;
    default:
        return err(LX_EBADF);
    }
    uint64_t pos = off < 0 ? o->off : (uint64_t)off;
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
        int64_t got = memfs_read(&g_fs, o->ino, pos + done, tmp, n);
        if (got < 0) return done ? (int64_t)done : got;
        if (got == 0) break;
        if (!uaccess_write(p->address_space, buf + done, tmp, (uint64_t)got)) {
            return done ? (int64_t)done : err(LX_EFAULT);
        }
        done += (uint64_t)got;
        if ((uint64_t)got < n) break;
    }
    if (off < 0) o->off += done;
    return (int64_t)done;
}

static int64_t do_write(process_t* p, ofile_t* o, uint64_t buf, uint64_t len, int64_t off) {
    if ((o->flags & O_ACCMODE) == 0 && o->kind != OF_CONSOLE) return err(LX_EBADF);
    switch (o->kind) {
    case OF_CONSOLE: return write_console(p, buf, len);
    case OF_NULL: case OF_ZERO: case OF_RANDOM: return (int64_t)len;
    case OF_DIR: return err(LX_EISDIR);
    case OF_FILE: break;
    default: return err(LX_EBADF);
    }
    uint64_t pos;
    if (off >= 0) {
        pos = (uint64_t)off;
    } else {
        if (o->flags & O_APPEND) {
            memfs_stat_t st;
            memfs_stat(&g_fs, o->ino, &st);
            o->off = st.size;
        }
        pos = o->off;
    }
    uint8_t tmp[512];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
        if (!uaccess_read(p->address_space, buf + done, tmp, n)) return done ? (int64_t)done : err(LX_EFAULT);
        int64_t w = memfs_write(&g_fs, o->ino, pos + done, tmp, n);
        if (w < 0) return done ? (int64_t)done : w;
        done += (uint64_t)w;
    }
    if (off < 0) o->off += done;
    return (int64_t)done;
}

static int64_t sys_rw(process_t* p, int64_t fd, uint64_t buf, uint64_t len, int64_t off, bool write) {
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    if (off >= 0 && o->kind != OF_FILE) return err(LX_ESPIPE);
    return write ? do_write(p, o, buf, len, off) : do_read(p, o, buf, len, off);
}

static int64_t sys_rwv(process_t* p, int64_t fd, uint64_t iov, uint64_t cnt, bool write) {
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    if (cnt > 1024) return err(LX_EINVAL);
    int64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t v[2];
        if (!uaccess_read(p->address_space, iov + 16 * i, v, sizeof(v))) return total ? total : err(LX_EFAULT);
        if (!v[1]) continue;
        int64_t n = write ? do_write(p, o, v[0], v[1], -1) : do_read(p, o, v[0], v[1], -1);
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint64_t)n < v[1]) break;
    }
    return total;
}

/* ---- Opening ---- */

static int64_t sys_openat(process_t* p, int64_t dirfd, uint64_t upath, uint64_t flags, uint64_t mode) {
    char path[MEMFS_PATH_MAX];
    int64_t rc = get_path(p, upath, path, sizeof(path));
    if (rc < 0) return rc;
    bool cloexec = (flags & O_CLOEXEC) != 0;
    uint32_t acc = (uint32_t)flags & O_ACCMODE;
    uint32_t keep = acc | ((uint32_t)flags & (O_APPEND | O_NONBLOCK));
    if ((flags & O_TMPFILE) == O_TMPFILE) return err(E_EOPNOTSUPP);
    uint32_t dk = dev_kind(path);
    if (dk) {
        int32_t of = of_alloc(dk, 0, keep);
        if (of < 0) return err(LX_ENOMEM);
        int64_t fd = fd_install(p, of, 0, cloexec);
        if (fd < 0) of_put(of);
        return fd;
    }
    uint32_t dir;
    if ((rc = base_dir(p, dirfd, path, &dir)) < 0) return rc;
    uint32_t ino;
    int r = memfs_lookup(&g_fs, dir, path, !(flags & O_NOFOLLOW), &ino);
    if (r == 0 && (flags & O_CREAT) && (flags & O_EXCL)) return err(LX_EEXIST);
    if (r == -MEMFS_ENOENT && (flags & O_CREAT)) {
        r = memfs_create(&g_fs, dir, path, MEMFS_REG, (uint16_t)(mode & ~fdt(p)->umask & 07777), 0, &ino);
    }
    if (r < 0) return mfs_err(r);
    memfs_stat_t st;
    memfs_stat(&g_fs, ino, &st);
    if (st.type == MEMFS_LNK) return err(E_ELOOP);           /* O_NOFOLLOW on a link */
    if ((flags & O_DIRECTORY) && st.type != MEMFS_DIR) return err(LX_ENOTDIR);
    if (st.type == MEMFS_DIR && acc != 0) return err(LX_EISDIR);
    if ((flags & O_TRUNC) && st.type == MEMFS_REG && acc != 0) {
        if ((r = memfs_truncate(&g_fs, ino, 0)) < 0) return mfs_err(r);
    }
    int32_t of = of_alloc(st.type == MEMFS_DIR ? OF_DIR : OF_FILE, ino, keep);
    if (of < 0) return err(LX_ENOMEM);
    int64_t fd = fd_install(p, of, 0, cloexec);
    if (fd < 0) of_put(of);
    return fd;
}

static int64_t sys_close(process_t* p, int64_t fd) {
    if (!of_get(p, fd)) return err(LX_EBADF);
    fd_close(p, fd);
    return 0;
}

static int64_t sys_dup3(process_t* p, int64_t oldfd, int64_t newfd, uint64_t flags, bool dup2) {
    ofile_t* o = of_get(p, oldfd);
    if (!o) return err(LX_EBADF);
    if (newfd < 0 || newfd >= MAX_FDS) return err(LX_EBADF);
    if (oldfd == newfd) return dup2 ? newfd : err(LX_EINVAL);
    if (flags & ~(uint64_t)O_CLOEXEC) return err(LX_EINVAL);
    fdt_t* t = fdt(p);
    if (t->of[newfd] >= 0) fd_close(p, newfd);
    t->of[newfd] = t->of[oldfd];
    t->cloexec[newfd] = (flags & O_CLOEXEC) != 0;
    o->refs++;
    return newfd;
}

static int64_t sys_dup(process_t* p, int64_t fd, int64_t lowest, bool cloexec) {
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    int64_t n = fd_install(p, fdt(p)->of[fd], lowest, cloexec);
    if (n >= 0) o->refs++;
    return n;
}

/* ---- Metadata ---- */

static void fill_stat(const memfs_stat_t* s, uint64_t out[18]) {
    memset(out, 0, 18 * sizeof(uint64_t));
    uint32_t type = s->type == MEMFS_DIR ? S_IFDIR : s->type == MEMFS_LNK ? S_IFLNK : S_IFREG;
    out[0] = 1;                                  /* st_dev */
    out[1] = s->ino;
    out[2] = s->nlink;
    out[3] = (uint64_t)(type | s->mode);          /* st_mode, st_uid = 0 */
    /* out[4]: st_gid, pad; out[5]: st_rdev */
    out[6] = s->size;
    out[7] = 4096;                               /* st_blksize */
    out[8] = s->blocks;
    out[9] = s->mtime_ns / 1000000000ULL;  out[10] = s->mtime_ns % 1000000000ULL;   /* atime */
    out[11] = s->mtime_ns / 1000000000ULL; out[12] = s->mtime_ns % 1000000000ULL;   /* mtime */
    out[13] = s->ctime_ns / 1000000000ULL; out[14] = s->ctime_ns % 1000000000ULL;   /* ctime */
}

static void dev_stat(uint32_t kind, uint64_t out[18]) {
    memset(out, 0, 18 * sizeof(uint64_t));
    out[0] = 5;
    out[1] = 1000 + kind;
    out[2] = 1;
    out[3] = S_IFCHR | 0666;
    out[5] = kind == OF_CONSOLE ? 0x0501 : 0x0103;
    out[7] = 4096;
}

static int64_t stat_ofile(ofile_t* o, uint64_t st[18]) {
    if (o->kind == OF_FILE || o->kind == OF_DIR) {
        memfs_stat_t s;
        int r = memfs_stat(&g_fs, o->ino, &s);
        if (r < 0) return mfs_err(r);
        fill_stat(&s, st);
    } else {
        dev_stat(o->kind, st);
    }
    return 0;
}

static int64_t sys_fstatat(process_t* p, int64_t dirfd, uint64_t upath, uint64_t ust, uint64_t flags) {
    uint64_t st[18];
    char path[MEMFS_PATH_MAX];
    int64_t rc = upath ? get_path(p, upath, path, sizeof(path)) : err(LX_ENOENT);
    if (rc == err(LX_ENOENT) && (flags & AT_EMPTY_PATH)) {
        ofile_t* o = (int32_t)dirfd == AT_FDCWD ? 0 : of_get(p, dirfd);
        if ((int32_t)dirfd == AT_FDCWD) {
            memfs_stat_t s;
            memfs_stat(&g_fs, fdt(p)->cwd, &s);
            fill_stat(&s, st);
        } else if (!o) {
            return err(LX_EBADF);
        } else if ((rc = stat_ofile(o, st)) < 0) {
            return rc;
        }
    } else {
        if (rc < 0) return rc;
        uint32_t dk = dev_kind(path);
        if (dk) {
            dev_stat(dk, st);
        } else {
            uint32_t dir, ino;
            if ((rc = base_dir(p, dirfd, path, &dir)) < 0) return rc;
            int r = memfs_lookup(&g_fs, dir, path, !(flags & AT_SYMLINK_NOFOLLOW), &ino);
            if (r < 0) return mfs_err(r);
            memfs_stat_t s;
            memfs_stat(&g_fs, ino, &s);
            fill_stat(&s, st);
        }
    }
    return uaccess_write(p->address_space, ust, st, 144) ? 0 : err(LX_EFAULT);
}

static int64_t sys_fstat(process_t* p, int64_t fd, uint64_t ust) {
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    uint64_t st[18];
    int64_t rc = stat_ofile(o, st);
    if (rc < 0) return rc;
    return uaccess_write(p->address_space, ust, st, 144) ? 0 : err(LX_EFAULT);
}

static int64_t sys_statx(process_t* p, int64_t dirfd, uint64_t upath, uint64_t flags, uint64_t mask,
                         uint64_t ubuf) {
    (void)mask;
    uint64_t st[18];
    /* Reuse fstatat into a kernel buffer, then repack. */
    char path[MEMFS_PATH_MAX];
    int64_t rc = get_path(p, upath, path, sizeof(path));
    if (rc < 0 && !(rc == err(LX_ENOENT) && (flags & AT_EMPTY_PATH))) return rc;
    if (rc < 0) {
        ofile_t* o = of_get(p, dirfd);
        if (!o) return err(LX_EBADF);
        if ((rc = stat_ofile(o, st)) < 0) return rc;
    } else {
        uint32_t dk = dev_kind(path);
        if (dk) {
            dev_stat(dk, st);
        } else {
            uint32_t dir, ino;
            if ((rc = base_dir(p, dirfd, path, &dir)) < 0) return rc;
            int r = memfs_lookup(&g_fs, dir, path, !(flags & AT_SYMLINK_NOFOLLOW), &ino);
            if (r < 0) return mfs_err(r);
            memfs_stat_t s;
            memfs_stat(&g_fs, ino, &s);
            fill_stat(&s, st);
        }
    }
    uint8_t x[256];
    memset(x, 0, sizeof(x));
    uint32_t u32;
    uint64_t u64;
    u32 = 0x7FF; memcpy(x + 0, &u32, 4);                         /* stx_mask: basic stats */
    u32 = 4096; memcpy(x + 4, &u32, 4);                          /* stx_blksize */
    u32 = (uint32_t)st[2]; memcpy(x + 16, &u32, 4);              /* stx_nlink */
    uint16_t m16 = (uint16_t)st[3]; memcpy(x + 28, &m16, 2);     /* stx_mode */
    u64 = st[1]; memcpy(x + 32, &u64, 8);                        /* stx_ino */
    u64 = st[6]; memcpy(x + 40, &u64, 8);                        /* stx_size */
    u64 = st[8]; memcpy(x + 48, &u64, 8);                        /* stx_blocks */
    for (int t = 0; t < 4; t++) {                                /* atime, btime, ctime, mtime */
        uint64_t sec = t == 2 ? st[13] : st[11], nsec = t == 2 ? st[14] : st[12];
        memcpy(x + 64 + 16 * t, &sec, 8);
        u32 = (uint32_t)nsec;
        memcpy(x + 72 + 16 * t, &u32, 4);
    }
    return uaccess_write(p->address_space, ubuf, x, sizeof(x)) ? 0 : err(LX_EFAULT);
}

static int64_t sys_lseek(process_t* p, int64_t fd, int64_t off, uint64_t whence) {
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    if (o->kind == OF_CONSOLE) return err(LX_ESPIPE);
    if (o->kind != OF_FILE && o->kind != OF_DIR) return 0;
    memfs_stat_t st;
    memfs_stat(&g_fs, o->ino, &st);
    int64_t base;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = (int64_t)o->off; break;
    case 2: base = (int64_t)st.size; break;
    case 3: case 4:                                /* SEEK_DATA, SEEK_HOLE: no holes reported */
        if (off < 0 || (uint64_t)off >= st.size) return err(6 /* ENXIO */);
        return whence == 3 ? off : (int64_t)st.size;
    default: return err(LX_EINVAL);
    }
    if (base + off < 0) return err(LX_EINVAL);
    o->off = (uint64_t)(base + off);
    return (int64_t)o->off;
}

static int64_t sys_getdents64(process_t* p, int64_t fd, uint64_t buf, uint64_t len) {
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    if (o->kind != OF_DIR) return err(LX_ENOTDIR);
    uint64_t done = 0;
    for (;;) {
        uint32_t ino, nl;
        const char* name;
        if (memfs_dirent(&g_fs, o->ino, (uint32_t)o->off, &ino, &name, &nl) < 0) break;
        uint32_t reclen = (uint32_t)((19 + nl + 1 + 7) & ~7u);
        if (done + reclen > len) {
            if (done == 0) return err(LX_EINVAL);
            break;
        }
        uint8_t rec[19 + MEMFS_NAME_MAX + 8];
        memset(rec, 0, reclen);
        uint64_t d_ino = ino, d_off = o->off + 1;
        uint16_t rl = (uint16_t)reclen;
        memfs_stat_t st;
        memfs_stat(&g_fs, ino, &st);
        uint8_t type = st.type == MEMFS_DIR ? 4 : st.type == MEMFS_LNK ? 10 : 8;
        memcpy(rec, &d_ino, 8);
        memcpy(rec + 8, &d_off, 8);
        memcpy(rec + 16, &rl, 2);
        rec[18] = type;
        memcpy(rec + 19, name, nl);
        if (!uaccess_write(p->address_space, buf + done, rec, reclen)) return err(LX_EFAULT);
        done += reclen;
        o->off++;
    }
    return (int64_t)done;
}

static int64_t sys_fcntl(process_t* p, int64_t fd, uint64_t cmd, uint64_t arg) {
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    fdt_t* t = fdt(p);
    switch (cmd) {
    case 0:    return sys_dup(p, fd, (int64_t)arg, false);       /* F_DUPFD */
    case 1030: return sys_dup(p, fd, (int64_t)arg, true);        /* F_DUPFD_CLOEXEC */
    case 1:    return t->cloexec[fd] ? 1 : 0;                    /* F_GETFD */
    case 2:    t->cloexec[fd] = (arg & 1) != 0; return 0;        /* F_SETFD */
    case 3:    return o->flags;                                  /* F_GETFL */
    case 4:                                                      /* F_SETFL */
        o->flags = (o->flags & O_ACCMODE) | ((uint32_t)arg & (O_APPEND | O_NONBLOCK));
        return 0;
    case 5: {                                                    /* F_GETLK: nothing conflicts */
        int16_t unlck = 2;
        return uaccess_write(p->address_space, arg, &unlck, 2) ? 0 : err(LX_EFAULT);
    }
    case 6: case 7:                                              /* F_SETLK(W): one machine */
    case 36: case 37: case 38:                                   /* OFD locks */
        return 0;
    default:
        return err(LX_EINVAL);
    }
}

static int64_t sys_getcwd(process_t* p, uint64_t buf, uint64_t size) {
    char path[MEMFS_PATH_MAX];
    int n = memfs_path(&g_fs, fdt(p)->cwd, path, sizeof(path));
    if (n < 0) return n;
    if ((uint64_t)n + 1 > size) return err(LX_ERANGE);
    return uaccess_write(p->address_space, buf, path, (uint64_t)n + 1) ? n + 1 : err(LX_EFAULT);
}

static int64_t set_cwd(process_t* p, uint32_t ino) {
    memfs_stat_t st;
    if (memfs_stat(&g_fs, ino, &st) < 0) return err(LX_ENOENT);
    if (st.type != MEMFS_DIR) return err(LX_ENOTDIR);
    fdt(p)->cwd = ino;
    return 0;
}

static int64_t sys_chdir(process_t* p, uint64_t upath) {
    char path[MEMFS_PATH_MAX];
    int64_t rc = get_path(p, upath, path, sizeof(path));
    if (rc < 0) return rc;
    uint32_t ino;
    int r = memfs_lookup(&g_fs, fdt(p)->cwd, path, true, &ino);
    return r < 0 ? mfs_err(r) : set_cwd(p, ino);
}

/* The *at() calls that take one path. */
static int64_t at_path(process_t* p, int64_t dirfd, uint64_t upath, char* path, uint32_t* dir) {
    int64_t rc = get_path(p, upath, path, MEMFS_PATH_MAX);
    if (rc < 0) return rc;
    if (dev_kind(path)) return err(E_EROFS);
    return base_dir(p, dirfd, path, dir);
}

static int64_t sys_mkdirat(process_t* p, int64_t dirfd, uint64_t upath, uint64_t mode) {
    char path[MEMFS_PATH_MAX];
    uint32_t dir;
    int64_t rc = at_path(p, dirfd, upath, path, &dir);
    if (rc < 0) return rc;
    return mfs_err(memfs_create(&g_fs, dir, path, MEMFS_DIR,
                                (uint16_t)(mode & ~fdt(p)->umask & 07777), 0, 0));
}

static int64_t sys_unlinkat(process_t* p, int64_t dirfd, uint64_t upath, uint64_t flags) {
    char path[MEMFS_PATH_MAX];
    uint32_t dir;
    int64_t rc = at_path(p, dirfd, upath, path, &dir);
    if (rc < 0) return rc;
    return mfs_err(memfs_unlink(&g_fs, dir, path, (flags & AT_REMOVEDIR) != 0));
}

static int64_t sys_renameat(process_t* p, int64_t ofd, uint64_t from, int64_t nfd, uint64_t to,
                            uint64_t flags) {
    char a[MEMFS_PATH_MAX], b[MEMFS_PATH_MAX];
    uint32_t da, db;
    int64_t rc = at_path(p, ofd, from, a, &da);
    if (rc < 0) return rc;
    if ((rc = at_path(p, nfd, to, b, &db)) < 0) return rc;
    if (flags & ~1ULL) return err(LX_EINVAL);                    /* only RENAME_NOREPLACE */
    uint32_t ino;
    if ((flags & 1) && memfs_lookup(&g_fs, db, b, false, &ino) == 0) return err(LX_EEXIST);
    if (da != db) {
        /* memfs resolves both from one directory: make them absolute. */
        char abs_a[MEMFS_PATH_MAX], abs_b[MEMFS_PATH_MAX];
        const char* pa = a;
        const char* pb = b;
        if (a[0] != '/') {
            int n = memfs_path(&g_fs, da, abs_a, sizeof(abs_a) - 1 - MEMFS_NAME_MAX);
            if (n < 0) return n;
            uint32_t i = 0;
            if (n > 1) abs_a[n++] = '/';
            while (a[i] && (uint32_t)n < sizeof(abs_a) - 1) abs_a[n++] = a[i++];
            abs_a[n] = 0;
            pa = abs_a;
        }
        if (b[0] != '/') {
            int n = memfs_path(&g_fs, db, abs_b, sizeof(abs_b) - 1 - MEMFS_NAME_MAX);
            if (n < 0) return n;
            uint32_t i = 0;
            if (n > 1) abs_b[n++] = '/';
            while (b[i] && (uint32_t)n < sizeof(abs_b) - 1) abs_b[n++] = b[i++];
            abs_b[n] = 0;
            pb = abs_b;
        }
        return mfs_err(memfs_rename(&g_fs, MEMFS_ROOT, pa, pb));
    }
    return mfs_err(memfs_rename(&g_fs, da, a, b));
}

static int64_t sys_linkat(process_t* p, int64_t ofd, uint64_t from, int64_t nfd, uint64_t to) {
    char a[MEMFS_PATH_MAX], b[MEMFS_PATH_MAX];
    uint32_t da, db;
    int64_t rc = at_path(p, ofd, from, a, &da);
    if (rc < 0) return rc;
    if ((rc = at_path(p, nfd, to, b, &db)) < 0) return rc;
    if (da != db && (a[0] != '/' || b[0] != '/')) return err(E_EXDEV);  /* rare; keep simple */
    return mfs_err(memfs_link(&g_fs, da, a, b));
}

static int64_t sys_symlinkat(process_t* p, uint64_t utarget, int64_t dirfd, uint64_t upath) {
    char target[MEMFS_PATH_MAX], path[MEMFS_PATH_MAX];
    int64_t rc = get_path(p, utarget, target, sizeof(target));
    if (rc < 0) return rc;
    uint32_t dir;
    if ((rc = at_path(p, dirfd, upath, path, &dir)) < 0) return rc;
    return mfs_err(memfs_create(&g_fs, dir, path, MEMFS_LNK, 0777, target, 0));
}

static int64_t sys_readlinkat(process_t* p, int64_t dirfd, uint64_t upath, uint64_t buf, uint64_t size) {
    char path[MEMFS_PATH_MAX];
    uint32_t dir, ino;
    int64_t rc = get_path(p, upath, path, sizeof(path));
    if (rc < 0) return rc;
    if ((rc = base_dir(p, dirfd, path, &dir)) < 0) return rc;
    int r = memfs_lookup(&g_fs, dir, path, false, &ino);
    if (r < 0) return mfs_err(r);
    char out[MEMFS_PATH_MAX];
    r = memfs_readlink(&g_fs, ino, out, (uint32_t)(size < sizeof(out) ? size : sizeof(out)));
    if (r < 0) return mfs_err(r);
    return uaccess_write(p->address_space, buf, out, (uint64_t)r) ? r : err(LX_EFAULT);
}

static int64_t sys_faccessat(process_t* p, int64_t dirfd, uint64_t upath, uint64_t flags) {
    char path[MEMFS_PATH_MAX];
    uint32_t dir, ino;
    int64_t rc = get_path(p, upath, path, sizeof(path));
    if (rc < 0) return rc;
    if (dev_kind(path)) return 0;
    if ((rc = base_dir(p, dirfd, path, &dir)) < 0) return rc;
    /* Everyone is root: existence is the only check. */
    return mfs_err(memfs_lookup(&g_fs, dir, path, !(flags & AT_SYMLINK_NOFOLLOW), &ino));
}

static int64_t sys_truncate_path(process_t* p, uint64_t upath, int64_t len) {
    char path[MEMFS_PATH_MAX];
    uint32_t ino;
    int64_t rc = get_path(p, upath, path, sizeof(path));
    if (rc < 0) return rc;
    if (len < 0) return err(LX_EINVAL);
    int r = memfs_lookup(&g_fs, fdt(p)->cwd, path, true, &ino);
    return r < 0 ? mfs_err(r) : mfs_err(memfs_truncate(&g_fs, ino, (uint64_t)len));
}

static int64_t sys_ftruncate(process_t* p, int64_t fd, int64_t len) {
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    if (o->kind != OF_FILE || (o->flags & O_ACCMODE) == 0 || len < 0) return err(LX_EINVAL);
    return mfs_err(memfs_truncate(&g_fs, o->ino, (uint64_t)len));
}

static int64_t sys_chmodat(process_t* p, int64_t dirfd, uint64_t upath, uint64_t mode) {
    char path[MEMFS_PATH_MAX];
    uint32_t dir, ino;
    int64_t rc = at_path(p, dirfd, upath, path, &dir);
    if (rc < 0) return rc;
    int r = memfs_lookup(&g_fs, dir, path, true, &ino);
    return r < 0 ? mfs_err(r) : mfs_err(memfs_chmod(&g_fs, ino, (uint16_t)mode));
}

static int64_t sys_utimensat(process_t* p, int64_t dirfd, uint64_t upath, uint64_t times, uint64_t flags) {
    uint32_t ino;
    if (!upath) {
        ofile_t* o = of_get(p, dirfd);
        if (!o || (o->kind != OF_FILE && o->kind != OF_DIR)) return err(LX_EBADF);
        ino = o->ino;
    } else {
        char path[MEMFS_PATH_MAX];
        uint32_t dir;
        int64_t rc = at_path(p, dirfd, upath, path, &dir);
        if (rc < 0) return rc;
        int r = memfs_lookup(&g_fs, dir, path, !(flags & AT_SYMLINK_NOFOLLOW), &ino);
        if (r < 0) return mfs_err(r);
    }
    uint64_t t = g_fs_now;
    if (times) {
        uint64_t ts[4];
        if (!uaccess_read(p->address_space, times, ts, sizeof(ts))) return err(LX_EFAULT);
        if (ts[3] == ((1ULL << 30) - 2)) return 0;               /* UTIME_OMIT */
        if (ts[3] != ((1ULL << 30) - 1)) t = ts[2] * 1000000000ULL + ts[3];   /* not UTIME_NOW */
    }
    return mfs_err(memfs_set_mtime(&g_fs, ino, t));
}

static int64_t sys_statfs(process_t* p, uint64_t buf) {
    uint64_t s[15];
    memset(s, 0, sizeof(s));
    s[0] = 0x01021994;          /* TMPFS_MAGIC */
    s[1] = 4096;                /* f_bsize */
    s[2] = s[3] = s[4] = 1u << 20;
    s[5] = s[6] = 1u << 20;     /* files, ffree */
    s[8] = 255;                 /* f_namelen */
    s[9] = 4096;                /* f_frsize */
    return uaccess_write(p->address_space, buf, s, 120) ? 0 : err(LX_EFAULT);
}

/* ---- File mappings ---- */

#define MAP_ANONYMOUS 0x20

static int64_t sys_mmap_file(process_t* p, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
                             int64_t fd, uint64_t off) {
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    if (o->kind == OF_ZERO || o->kind == OF_NULL) {
        return vm_mmap(p, addr, len, prot, (flags & ~0x03ULL) | 0x02 | MAP_ANONYMOUS, -1, 0);
    }
    if (o->kind != OF_FILE) return err(19 /* ENODEV */);
    if (off & 0xFFF) return err(LX_EINVAL);
    /* MAP_SHARED of a file only works read-only: then it reads like a
     * private copy. Writable shared mappings are not offered. */
    if ((flags & 0x03) == 0x01 && (prot & LX_PROT_WRITE)) return err(19 /* ENODEV */);
    if ((o->flags & O_ACCMODE) == O_WRONLY) return err(LX_EACCES);
    /* A private copy of the file's pages, made now: map writable, fill, then
     * set the protection asked for. */
    uint64_t f = (flags & ~0x03ULL) | 0x02 | MAP_ANONYMOUS;
    int64_t at = vm_mmap(p, addr, len, prot | LX_PROT_WRITE | LX_PROT_READ, f, -1, 0);
    if (at < 0) return at;
    memfs_stat_t st;
    memfs_stat(&g_fs, o->ino, &st);
    uint8_t tmp[512];
    for (uint64_t done = 0; off + done < st.size && done < len; ) {
        uint64_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
        int64_t got = memfs_read(&g_fs, o->ino, off + done, tmp, n);
        if (got <= 0) break;
        if (!uaccess_write(p->address_space, (uint64_t)at + done, tmp, (uint64_t)got)) break;
        done += (uint64_t)got;
    }
    if (!(prot & LX_PROT_WRITE) || !(prot & LX_PROT_READ)) vm_mprotect(p, (uint64_t)at, len, prot);
    return at;
}

/* ---- Dispatch ---- */

static int64_t ioctl(process_t* p, int64_t fd, uint64_t req, uint64_t arg) {
    (void)arg;
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    (void)req;
    return err(LX_ENOTTY);           /* no terminal yet (#249); files are not ttys */
}

bool files_syscall(process_t* p, uint64_t nr, int64_t* ret) {
    process_context_t* c = &p->context;
    uint64_t a0 = c->rdi, a1 = c->rsi, a2 = c->rdx, a3 = c->r10, a4 = c->r8, a5 = c->r9;
    if (!fdt(p)) return false;
    int64_t r;
    switch (nr) {
    case 0:   r = sys_rw(p, (int64_t)a0, a1, a2, -1, false); break;              /* read */
    case 1:   r = sys_rw(p, (int64_t)a0, a1, a2, -1, true); break;               /* write */
    case 2:   r = sys_openat(p, AT_FDCWD, a0, a1, a2); break;                    /* open */
    case 85:  r = sys_openat(p, AT_FDCWD, a0, O_CREAT | O_WRONLY | O_TRUNC, a1); break;   /* creat */
    case 257: r = sys_openat(p, (int64_t)a0, a1, a2, a3); break;                 /* openat */
    case 3:   r = sys_close(p, (int64_t)a0); break;
    case 4:   r = sys_fstatat(p, AT_FDCWD, a0, a1, 0); break;                    /* stat */
    case 5:   r = sys_fstat(p, (int64_t)a0, a1); break;
    case 6:   r = sys_fstatat(p, AT_FDCWD, a0, a1, AT_SYMLINK_NOFOLLOW); break;  /* lstat */
    case 262: r = sys_fstatat(p, (int64_t)a0, a1, a2, a3); break;                /* newfstatat */
    case 332: r = sys_statx(p, (int64_t)a0, a1, a2, a3, a4); break;
    case 8:   r = sys_lseek(p, (int64_t)a0, (int64_t)a1, a2); break;
    case 9:                                                                      /* mmap */
        if (a3 & MAP_ANONYMOUS) return false;
        r = sys_mmap_file(p, a0, a1, a2, a3, (int64_t)a4, a5);
        break;
    case 16:  r = ioctl(p, (int64_t)a0, a1, a2); break;
    case 17:  r = (int64_t)a3 < 0 ? err(LX_EINVAL) : sys_rw(p, (int64_t)a0, a1, a2, (int64_t)a3, false);
              break;                                                             /* pread64 */
    case 18:  r = (int64_t)a3 < 0 ? err(LX_EINVAL) : sys_rw(p, (int64_t)a0, a1, a2, (int64_t)a3, true);
              break;                                                             /* pwrite64 */
    case 19:  r = sys_rwv(p, (int64_t)a0, a1, a2, false); break;                 /* readv */
    case 20:  r = sys_rwv(p, (int64_t)a0, a1, a2, true); break;                  /* writev */
    case 21:  r = sys_faccessat(p, AT_FDCWD, a0, 0); break;                      /* access */
    case 269: case 439: r = sys_faccessat(p, (int64_t)a0, a1, nr == 439 ? a3 : 0); break;
    case 32:  r = sys_dup(p, (int64_t)a0, 0, false); break;
    case 33:  r = sys_dup3(p, (int64_t)a0, (int64_t)a1, 0, true); break;         /* dup2 */
    case 292: r = sys_dup3(p, (int64_t)a0, (int64_t)a1, a2, false); break;       /* dup3 */
    case 72:  r = sys_fcntl(p, (int64_t)a0, a1, a2); break;
    case 73:                                                                     /* flock */
    case 74: case 75:                                                            /* fsync, fdatasync */
    case 221:                                                                    /* fadvise64 */
        r = of_get(p, (int64_t)a0) ? 0 : err(LX_EBADF);
        break;
    case 162: case 306: r = 0; break;                                            /* sync, syncfs */
    case 76:  r = sys_truncate_path(p, a0, (int64_t)a1); break;
    case 77:  r = sys_ftruncate(p, (int64_t)a0, (int64_t)a1); break;
    case 217: r = sys_getdents64(p, (int64_t)a0, a1, a2); break;
    case 79:  r = sys_getcwd(p, a0, a1); break;
    case 80:  r = sys_chdir(p, a0); break;
    case 81: {                                                                   /* fchdir */
        ofile_t* o = of_get(p, (int64_t)a0);
        r = !o ? err(LX_EBADF) : o->kind != OF_DIR ? err(LX_ENOTDIR) : set_cwd(p, o->ino);
        break;
    }
    case 82:  r = sys_renameat(p, AT_FDCWD, a0, AT_FDCWD, a1, 0); break;          /* rename */
    case 264: r = sys_renameat(p, (int64_t)a0, a1, (int64_t)a2, a3, 0); break;   /* renameat */
    case 316: r = sys_renameat(p, (int64_t)a0, a1, (int64_t)a2, a3, a4); break;  /* renameat2 */
    case 83:  r = sys_mkdirat(p, AT_FDCWD, a0, a1); break;                       /* mkdir */
    case 258: r = sys_mkdirat(p, (int64_t)a0, a1, a2); break;
    case 84:  r = sys_unlinkat(p, AT_FDCWD, a0, AT_REMOVEDIR); break;            /* rmdir */
    case 87:  r = sys_unlinkat(p, AT_FDCWD, a0, 0); break;                       /* unlink */
    case 263: r = sys_unlinkat(p, (int64_t)a0, a1, a2); break;
    case 86:  r = sys_linkat(p, AT_FDCWD, a0, AT_FDCWD, a1); break;              /* link */
    case 265: r = sys_linkat(p, (int64_t)a0, a1, (int64_t)a2, a3); break;
    case 88:  r = sys_symlinkat(p, a0, AT_FDCWD, a1); break;                     /* symlink */
    case 266: r = sys_symlinkat(p, a0, (int64_t)a1, a2); break;
    case 89:  r = sys_readlinkat(p, AT_FDCWD, a0, a1, a2); break;                /* readlink */
    case 267: r = sys_readlinkat(p, (int64_t)a0, a1, a2, a3); break;
    case 90:  r = sys_chmodat(p, AT_FDCWD, a0, a1); break;                       /* chmod */
    case 268: r = sys_chmodat(p, (int64_t)a0, a1, a2); break;                    /* fchmodat */
    case 91: {                                                                   /* fchmod */
        ofile_t* o = of_get(p, (int64_t)a0);
        r = !o ? err(LX_EBADF) : (o->kind == OF_FILE || o->kind == OF_DIR)
                                     ? mfs_err(memfs_chmod(&g_fs, o->ino, (uint16_t)a1)) : 0;
        break;
    }
    case 92: case 93: case 94: case 260: r = 0; break;                           /* chown family */
    case 95: r = fdt(p)->umask; fdt(p)->umask = (uint32_t)a0 & 0777; break;      /* umask */
    case 280: r = sys_utimensat(p, (int64_t)a0, a1, a2, a3); break;
    case 137: r = sys_statfs(p, a1); break;                                      /* statfs */
    case 138: r = of_get(p, (int64_t)a0) ? sys_statfs(p, a1) : err(LX_EBADF); break;
    default:
        return false;
    }
    *ret = r;
    return true;
}

/* ---- Times ---- */

void files_note_time(uint64_t realtime_ns) { g_fs_now = realtime_ns; }
static uint64_t fs_now(void) { return g_fs_now; }

/* ---- Programs in the filesystem ---- */

const lp_image_t* files_find_image(const char* path) {
    if (!g_fs.inodes || path[0] != '/') return 0;
    uint32_t ino;
    if (memfs_lookup(&g_fs, MEMFS_ROOT, path, true, &ino) < 0) return 0;
    uint64_t len;
    const uint8_t* bytes = memfs_seed_bytes(&g_fs, ino, &len);
    if (!bytes) return 0;
    for (uint32_t i = 0; i < g_nseed_images; i++) {
        if (g_seed_images[i].start == bytes) return &g_seed_images[i];
    }
    if (g_nseed_images == sizeof(g_seed_images) / sizeof(g_seed_images[0])) return 0;
    lp_image_t* img = &g_seed_images[g_nseed_images];
    char* name = g_seed_paths[g_nseed_images++];
    uint32_t n = 0;
    while (path[n] && n < sizeof(g_seed_paths[0]) - 1) { name[n] = path[n]; n++; }
    name[n] = 0;
    img->name = name;
    img->start = bytes;
    img->end = bytes + len;
    img->linux_abi = true;
    return img;
}

/* ---- Keyframes: the tree ---- */

static void fs_save(ksf_writer_t* w, void* ctx) {
    (void)ctx;
    ksf_put_u64(w, g_fs_now);
    memfs_save(&g_fs, w);
}

static int fs_restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    g_fs_now = ksf_get_u64(r);
    int rc = memfs_restore(&g_fs, r, version);
    if (rc == -MEMFS_EXDEV) kprintf("files: the recording's seed image differs from this boot's\n");
    if (rc == 0 && kstate_take_perturb(KS_ID_FS)) {
        /* Test hook: one permission bit of / changes. */
        memfs_stat_t st;
        memfs_stat(&g_fs, MEMFS_ROOT, &st);
        memfs_chmod(&g_fs, MEMFS_ROOT, st.mode ^ 1);
    }
    return rc;
}

#define FS_PID_BASE 0x40000000u

static uint32_t page_crc(uint32_t ino, uint32_t index, uint64_t entry) {
    return kfp_page_crc(FS_PID_BASE + ino, (uint64_t)index * PAGE_SIZE, entry & ~0xFFFULL,
                        (entry & MEMFS_PAGE_KNOWN) ? PAGE_KF_KNOWN : 0);
}

static uint32_t fs_sum(void* ctx) {
    (void)ctx;
    return memfs_checksum(&g_fs, page_crc);
}

/* Pages for the keyframe page pool (#244). */
typedef struct {
    kfp_page_visit_fn fn;
    void*             ctx;
} visit_t;

static void visit_page(void* c, uint32_t ino, uint32_t index, uint64_t* entry) {
    visit_t* v = (visit_t*)c;
    v->fn(v->ctx, FS_PID_BASE + ino, (uint64_t)index * PAGE_SIZE, entry);
}

static void fs_pages(kfp_page_visit_fn fn, void* ctx) {
    visit_t v = { fn, ctx };
    memfs_for_each_page(&g_fs, visit_page, &v);
}

static int fs_attach(uint32_t pid, uint64_t va, uint64_t entry) {
    return memfs_attach_page(&g_fs, pid - FS_PID_BASE, (uint32_t)(va / PAGE_SIZE), entry);
}

/* ---- Keyframes: descriptions and tables ---- */

static void files_save(ksf_writer_t* w, void* ctx) {
    (void)ctx;
    uint32_t live = 0;
    for (uint32_t i = 0; i < g_nof; i++) live += g_of[i].refs != 0;
    ksf_put_u32(w, live);
    for (uint32_t i = 0; i < g_nof; i++) {
        ofile_t* o = &g_of[i];
        if (!o->refs) continue;
        ksf_put_u32(w, i);
        ksf_put_u32(w, o->refs);
        ksf_put_u32(w, o->kind);
        ksf_put_u32(w, o->ino);
        ksf_put_u32(w, o->flags);
        ksf_put_u64(w, o->off);
    }
    ksf_put_u32(w, proc_count());
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* p = proc_at(i);
        fdt_t* t = fdt(p);
        ksf_put_u32(w, (uint32_t)p->pid);
        ksf_put_u32(w, t ? 1 : 0);
        if (!t) continue;
        ksf_put_u32(w, t->cwd);
        ksf_put_u32(w, t->umask);
        uint32_t n = 0;
        for (uint32_t fd = 0; fd < MAX_FDS; fd++) n += t->of[fd] >= 0;
        ksf_put_u32(w, n);
        for (uint32_t fd = 0; fd < MAX_FDS; fd++) {
            if (t->of[fd] < 0) continue;
            ksf_put_u32(w, fd | ((uint32_t)t->cloexec[fd] << 31));
            ksf_put_u32(w, (uint32_t)t->of[fd]);
        }
    }
}

static int files_restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    if (version != 1) return -1;
    /* The process table was rebuilt without descriptor tables; descriptions
     * are rebuilt here, and the inodes' open counts came with the tree. */
    if (g_of) memset(g_of, 0, (uint64_t)g_nof * sizeof(ofile_t));
    uint32_t live = ksf_get_u32(r);
    for (uint32_t k = 0; k < live && r->ok; k++) {
        uint32_t id = ksf_get_u32(r);
        if (id > (1u << 20)) return -1;
        while (id >= g_nof) {
            uint32_t cap = g_nof ? g_nof * 2 : 64;
            ofile_t* n = (ofile_t*)kzalloc((uint64_t)cap * sizeof(ofile_t));
            if (!n) return -1;
            if (g_of) {
                memcpy(n, g_of, (uint64_t)g_nof * sizeof(ofile_t));
                kfree(g_of);
            }
            g_of = n;
            g_nof = cap;
        }
        ofile_t* o = &g_of[id];
        o->refs = ksf_get_u32(r);
        o->kind = ksf_get_u32(r);
        o->ino = ksf_get_u32(r);
        o->flags = ksf_get_u32(r);
        o->off = ksf_get_u64(r);
    }
    uint32_t nproc = ksf_get_u32(r);
    for (uint32_t k = 0; k < nproc && r->ok; k++) {
        process_t* p = proc_by_pid(ksf_get_u32(r));
        bool has = ksf_get_u32(r) != 0;
        if (!p) return -1;
        if (!has) continue;
        fdt_t* t = fdt_new();
        if (!t) return -1;
        p->files = t;
        t->cwd = ksf_get_u32(r);
        t->umask = ksf_get_u32(r);
        uint32_t n = ksf_get_u32(r);
        for (uint32_t i = 0; i < n && r->ok; i++) {
            uint32_t fd = ksf_get_u32(r);
            uint32_t of = ksf_get_u32(r);
            bool cx = (fd >> 31) != 0;
            fd &= 0x7FFFFFFF;
            if (fd >= MAX_FDS || of >= g_nof || !g_of[of].refs) return -1;
            t->of[fd] = (int32_t)of;
            t->cloexec[fd] = cx;
        }
    }
    return r->ok ? 0 : -1;
}

static uint32_t files_sum(void* ctx) {
    (void)ctx;
    uint32_t crc = 0;
    for (uint32_t i = 0; i < g_nof; i++) {
        ofile_t* o = &g_of[i];
        if (!o->refs) continue;
        uint64_t rec[5] = { i, o->refs, o->kind | ((uint64_t)o->ino << 32), o->flags, o->off };
        crc = divergence_checksum(crc, rec, sizeof(rec));
    }
    for (uint32_t i = 0; i < proc_count(); i++) {
        fdt_t* t = fdt(proc_at(i));
        if (!t) continue;
        uint32_t head[3] = { (uint32_t)proc_at(i)->pid, t->cwd, t->umask };
        crc = divergence_checksum(crc, head, sizeof(head));
        for (uint32_t fd = 0; fd < MAX_FDS; fd++) {
            if (t->of[fd] < 0) continue;
            uint32_t e[3] = { fd, (uint32_t)t->of[fd], t->cloexec[fd] };
            crc = divergence_checksum(crc, e, sizeof(e));
        }
    }
    return crc;
}

/* ---- Setup ---- */

static uint64_t env_alloc(void) { return pmm_alloc(); }
static void env_free(uint64_t f) { pmm_free(f); }
static void* env_ptr(uint64_t f) { return P2V(f); }

void files_init(const uint8_t* seed, uint64_t len, const char* seed_name) {
    static const memfs_env_t env = { env_alloc, env_free, env_ptr, tt_before_mapping_change, fs_now };
    g_fs_now = clock_base()->wall0_ns;
    int rc = memfs_init(&g_fs, &env, seed, len);
    if (rc < 0) {
        kprintf("files: the seed image %s is malformed (%d); starting empty\n", seed_name, rc);
        memfs_destroy(&g_fs);
        memfs_init(&g_fs, &env, 0, 0);
    } else if (seed) {
        kprintf("files: seed image %s: %u entries, %lu bytes, crc %08x\n", seed_name, g_fs.used - 1,
                len, g_fs.seed_crc);
    }
    static const ksf_subsys_t fs = { KS_ID_FS, "fs", 1, fs_save, fs_restore, fs_sum, 0 };
    static const ksf_subsys_t files = { KS_ID_FILES, "files", 1, files_save, files_restore, files_sum, 0 };
    if (kstate_register(&fs) != KSF_OK || kstate_register(&files) != KSF_OK) {
        panic("files: cannot register keyframe sections");
    }
    static const kfp_provider_t prov = { FS_PID_BASE, fs_pages, fs_attach };
    kfp_set_provider(&prov);
}
