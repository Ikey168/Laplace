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
#include "core/tty.h"
#include "core/ksig.h"
#include "core/net.h"
#include "memfs.h"
#include "entropy_record.h"
#include "divergence.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);

static inline int64_t err(int e) { return -(int64_t)e; }

/* Internal: the call would block (#249). files_syscall blocks the process
 * with the wait noted here; the kernel retries the call when a pipe or the
 * console changes, and wakes it with the result. */
#define E_BLOCK 512
static uint32_t g_block_reason;
static uint64_t g_block_obj, g_block_deadline;

static int64_t would_block(uint32_t reason, uint64_t obj) {
    g_block_reason = reason;
    g_block_obj = obj;
    g_block_deadline = 0;
    return err(E_BLOCK);
}

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

#define S_IFIFO  0010000
#define S_IFSOCK 0140000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFLNK  0120000

#define MAX_FDS  1024

/* ---- Open file descriptions ---- */

enum { OF_FREE = 0, OF_CONSOLE, OF_FILE, OF_DIR, OF_NULL, OF_ZERO, OF_RANDOM, OF_PIPE_R, OF_PIPE_W,
       OF_SOCKET, OF_EPOLL };

typedef struct {
    uint32_t refs;
    uint32_t kind;
    uint32_t ino;
    uint32_t flags;          /* access mode and status flags */
    uint64_t off;
} ofile_t;

typedef struct {
    uint32_t users;          /* processes sharing it: a thread group (#251) */
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
static lp_image_t g_seed_images[32]; /* programs and libraries loaded from the seed */
static char      g_seed_paths[32][96];
static uint32_t  g_nseed_images;

static fdt_t* fdt(process_t* p) { return (fdt_t*)p->files; }

/* ---- Pipes (#249) ---- */

#define PIPE_CAP 16384
#define PIPE_BUF 4096                /* writes up to this size are atomic */

typedef struct {
    bool     used;
    bool     rd, wr;                 /* the read and the write end are open */
    uint32_t len, head;              /* bytes buffered, from head (a ring) */
    uint8_t* buf;
} pipe_t;

static pipe_t*  g_pipes;
static uint32_t g_npipes;
static bool     g_changed;           /* a pipe or the console changed: retry the waiters */

static int32_t pipe_alloc(void) {
    uint32_t id = 0;
    while (id < g_npipes && g_pipes[id].used) id++;
    if (id == g_npipes) {
        uint32_t cap = g_npipes ? g_npipes * 2 : 16;
        pipe_t* n = (pipe_t*)kzalloc((uint64_t)cap * sizeof(pipe_t));
        if (!n) return -1;
        if (g_pipes) {
            memcpy(n, g_pipes, (uint64_t)g_npipes * sizeof(pipe_t));
            kfree(g_pipes);
        }
        g_pipes = n;
        g_npipes = cap;
    }
    pipe_t* q = &g_pipes[id];
    q->buf = (uint8_t*)kmalloc(PIPE_CAP);
    if (!q->buf) return -1;
    q->used = q->rd = q->wr = true;
    q->len = q->head = 0;
    return (int32_t)id;
}

/* ---- epoll (#253) ---- */

#define EP_MAX 64

typedef struct {
    int32_t  fd;
    uint32_t events;
    uint64_t data;
} ep_item_t;

typedef struct {
    bool      used;
    uint32_t  n;
    ep_item_t it[EP_MAX];
} epoll_t;

static epoll_t* g_ep;
static uint32_t g_nep;

static int32_t epoll_alloc(void) {
    uint32_t id = 0;
    while (id < g_nep && g_ep[id].used) id++;
    if (id == g_nep) {
        uint32_t cap = g_nep ? g_nep * 2 : 8;
        epoll_t* n = (epoll_t*)kzalloc((uint64_t)cap * sizeof(epoll_t));
        if (!n) return -1;
        if (g_ep) {
            memcpy(n, g_ep, (uint64_t)g_nep * sizeof(epoll_t));
            kfree(g_ep);
        }
        g_ep = n;
        g_nep = cap;
    }
    memset(&g_ep[id], 0, sizeof(epoll_t));
    g_ep[id].used = true;
    return (int32_t)id;
}

static void pipe_end_closed(uint32_t id, bool write_end) {
    pipe_t* q = &g_pipes[id];
    if (write_end) q->wr = false; else q->rd = false;
    g_changed = true;
    if (!q->rd && !q->wr) {
        kfree(q->buf);
        memset(q, 0, sizeof(*q));
    }
}

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
    if (o->kind == OF_PIPE_R || o->kind == OF_PIPE_W) pipe_end_closed(o->ino, o->kind == OF_PIPE_W);
    if (o->kind == OF_SOCKET) {
        net_close((int32_t)o->ino);
        g_changed = true;
    }
    if (o->kind == OF_EPOLL) memset(&g_ep[o->ino], 0, sizeof(epoll_t));
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
    t->users = 1;
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
    if (--t->users) {
        p->files = 0;            /* the rest of the group keeps it */
        return;
    }
    for (int64_t fd = 0; fd < MAX_FDS; fd++) {
        if (t->of[fd] >= 0) fd_close(p, fd);
    }
    kfree(t);
    p->files = 0;
    if (g_changed) files_retry_waiters();      /* a pipe end closed (#249) */
}

void files_fork(process_t* parent, process_t* child) {
    fdt_t* t = fdt_new();
    if (!t) panic("files: out of memory for a descriptor table");
    fdt_t* pt = fdt(parent);
    if (pt) {
        t->cwd = pt->cwd;
        t->umask = pt->umask;
        for (uint32_t fd = 0; fd < MAX_FDS; fd++) {
            if (pt->of[fd] < 0) continue;
            t->of[fd] = pt->of[fd];
            t->cloexec[fd] = pt->cloexec[fd];
            g_of[pt->of[fd]].refs++;
        }
    }
    child->files = t;
}

void files_share(process_t* parent, process_t* child) {
    fdt_t* t = fdt(parent);
    if (!t) {
        files_proc_new(child);
        return;
    }
    t->users++;
    child->files = t;
}

void files_exec(process_t* p) {
    fdt_t* t = fdt(p);
    if (!t) return;
    for (int64_t fd = 0; fd < MAX_FDS; fd++) {
        if (t->of[fd] >= 0 && t->cloexec[fd]) fd_close(p, fd);
    }
    if (g_changed) files_retry_waiters();
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

static int64_t pipe_read(process_t* p, ofile_t* o, uint64_t buf, uint64_t len) {
    pipe_t* q = &g_pipes[o->ino];
    if (!len) return 0;
    if (!q->len) {
        if (!q->wr) return 0;                                     /* end of file */
        return (o->flags & O_NONBLOCK) ? err(LX_EAGAIN) : would_block(WAIT_PIPE, o->ino);
    }
    uint64_t n = len < q->len ? len : q->len;
    uint64_t first = PIPE_CAP - q->head < n ? PIPE_CAP - q->head : n;
    if (!uaccess_write(p->address_space, buf, q->buf + q->head, first) ||
        (n > first && !uaccess_write(p->address_space, buf + first, q->buf, n - first))) {
        return err(LX_EFAULT);
    }
    q->head = (q->head + (uint32_t)n) % PIPE_CAP;
    q->len -= (uint32_t)n;
    g_changed = true;
    return (int64_t)n;
}

/* Up to PIPE_BUF bytes go in at once or not at all; more may go in part
 * (a short count, as with a signal: callers loop). */
static int64_t pipe_write(process_t* p, ofile_t* o, uint64_t buf, uint64_t len) {
    pipe_t* q = &g_pipes[o->ino];
    if (!q->rd) {
        ksig_send(p, SIG_SIGPIPE, 0, 0);
        return err(LX_EPIPE);
    }
    if (!len) return 0;
    uint32_t space = PIPE_CAP - q->len;
    if (!space || (len <= PIPE_BUF && space < len)) {
        return (o->flags & O_NONBLOCK) ? err(LX_EAGAIN) : would_block(WAIT_PIPE, o->ino);
    }
    uint64_t n = len < space ? len : space;
    uint32_t tail = (q->head + q->len) % PIPE_CAP;
    uint64_t first = PIPE_CAP - tail < n ? PIPE_CAP - tail : n;
    if (!uaccess_read(p->address_space, buf, q->buf + tail, first) ||
        (n > first && !uaccess_read(p->address_space, buf + first, q->buf, n - first))) {
        return err(LX_EFAULT);
    }
    q->len += (uint32_t)n;
    g_changed = true;
    return (int64_t)n;
}

/* ---- Sockets (#253): user memory in and out of core/net.h ---- */

#define MSG_PEEK     0x2
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
static uint8_t g_sockbuf[65536];

static int64_t sock_wait(ofile_t* o, uint32_t flags) {
    if ((o->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return err(LX_EAGAIN);
    return would_block(WAIT_NET, o->ino | (net_external((int32_t)o->ino) ? 1ULL << 32 : 0));
}

static bool get_sockaddr(process_t* p, uint64_t ua, uint64_t alen, net_addr_t* a, int64_t* e) {
    uint8_t sa[16];
    if (alen < 16 || !uaccess_read(p->address_space, ua, sa, 16)) {
        *e = alen < 16 ? err(LX_EINVAL) : err(LX_EFAULT);
        return false;
    }
    if ((sa[0] | sa[1] << 8) != 2) {                          /* AF_INET */
        *e = err(97);                                         /* EAFNOSUPPORT */
        return false;
    }
    a->port = (uint16_t)(sa[2] << 8 | sa[3]);
    a->ip = (uint32_t)sa[4] << 24 | (uint32_t)sa[5] << 16 | (uint32_t)sa[6] << 8 | sa[7];
    return true;
}

/* Write a sockaddr_in to (ua, *ualen) as accept/recvfrom/getsockname do. */
static int64_t put_sockaddr(process_t* p, uint64_t ua, uint64_t ualen, net_addr_t a) {
    if (!ua || !ualen) return 0;
    uint32_t cap;
    if (!uaccess_read(p->address_space, ualen, &cap, 4)) return err(LX_EFAULT);
    uint8_t sa[16] = { 2, 0, (uint8_t)(a.port >> 8), (uint8_t)a.port,
                       (uint8_t)(a.ip >> 24), (uint8_t)(a.ip >> 16), (uint8_t)(a.ip >> 8), (uint8_t)a.ip };
    uint32_t n = cap < 16 ? cap : 16, full = 16;
    if (!uaccess_write(p->address_space, ua, sa, n) || !uaccess_write(p->address_space, ualen, &full, 4)) {
        return err(LX_EFAULT);
    }
    return 0;
}

static int64_t sock_send(process_t* p, ofile_t* o, uint64_t buf, uint64_t len, uint32_t flags,
                         uint64_t ua, uint64_t alen) {
    net_addr_t to;
    int64_t e;
    if (ua && !get_sockaddr(p, ua, alen, &to, &e)) return e;
    if (len > sizeof(g_sockbuf)) len = sizeof(g_sockbuf);
    if (len && !uaccess_read(p->address_space, buf, g_sockbuf, len)) return err(LX_EFAULT);
    int64_t r = net_send((int32_t)o->ino, g_sockbuf, (uint32_t)len, ua ? &to : 0);
    g_changed |= net_changed();
    if (r == NET_EBLOCK) return sock_wait(o, flags);
    if (r == err(LX_EPIPE) && !(flags & MSG_NOSIGNAL)) ksig_send(p, SIG_SIGPIPE, 0, 0);
    return r;
}

static int64_t sock_recv(process_t* p, ofile_t* o, uint64_t buf, uint64_t len, uint32_t flags,
                         uint64_t ua, uint64_t ualen) {
    net_addr_t from = { 0, 0 };
    if (len > sizeof(g_sockbuf)) len = sizeof(g_sockbuf);
    int64_t r = net_recv((int32_t)o->ino, g_sockbuf, (uint32_t)len, &from, (flags & MSG_PEEK) != 0);
    g_changed |= net_changed();
    if (r == NET_EBLOCK) return sock_wait(o, flags);
    if (r < 0) return r;
    if (r && !uaccess_write(p->address_space, buf, g_sockbuf, (uint64_t)r)) return err(LX_EFAULT);
    int64_t e = put_sockaddr(p, ua, ualen, from);
    return e < 0 ? e : r;
}

/* Read `len` bytes at `off` (or the description's offset if off < 0). */
static int64_t do_read(process_t* p, ofile_t* o, uint64_t buf, uint64_t len, int64_t off) {
    if ((o->flags & O_ACCMODE) == O_WRONLY) return err(LX_EBADF);
    uint8_t tmp[512];
    switch (o->kind) {
    case OF_CONSOLE: {
        int64_t n = tty_read(p, buf, len);
        if (n != err(LX_EAGAIN)) return n;
        return (o->flags & O_NONBLOCK) ? n : would_block(WAIT_INPUT, 0);
    }
    case OF_PIPE_R:
        return pipe_read(p, o, buf, len);
    case OF_SOCKET:
        return sock_recv(p, o, buf, len, 0, 0, 0);
    case OF_NULL:
        return 0;
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
    if ((o->flags & O_ACCMODE) == 0 && o->kind != OF_CONSOLE && o->kind != OF_SOCKET) return err(LX_EBADF);
    switch (o->kind) {
    case OF_CONSOLE: return write_console(p, buf, len);
    case OF_PIPE_W: return pipe_write(p, o, buf, len);
    case OF_SOCKET: return sock_send(p, o, buf, len, 0, 0, 0);
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
    bool fifo = kind == OF_PIPE_R || kind == OF_PIPE_W;
    out[3] = kind == OF_SOCKET ? S_IFSOCK | 0777 : fifo ? S_IFIFO | 0600 : S_IFCHR | 0666;
    out[5] = fifo ? 0 : kind == OF_CONSOLE ? 0x0501 : 0x0103;
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
    if (o->kind == OF_CONSOLE || o->kind == OF_PIPE_R || o->kind == OF_PIPE_W || o->kind == OF_SOCKET) {
        return err(LX_ESPIPE);
    }
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
    ofile_t* o = of_get(p, fd);
    if (!o) return err(LX_EBADF);
    int64_t r;
    if (req == 0x5421) {                                  /* FIONBIO */
        int32_t on;
        if (!uaccess_read(p->address_space, arg, &on, 4)) return err(LX_EFAULT);
        o->flags = on ? o->flags | O_NONBLOCK : o->flags & ~(uint32_t)O_NONBLOCK;
        return 0;
    }
    if (o->kind == OF_CONSOLE && tty_ioctl(p, req, arg, &r)) return r;   /* the tty (#249) */
    if (req == 0x541B && o->kind == OF_PIPE_R) {           /* FIONREAD */
        int32_t n = (int32_t)g_pipes[o->ino].len;
        return uaccess_write(p->address_space, arg, &n, 4) ? 0 : err(LX_EFAULT);
    }
    return err(LX_ENOTTY);                                /* files and pipes are not ttys */
}

/* ---- pipe, poll, select (#249) ---- */

static int64_t sys_pipe2(process_t* p, uint64_t ufds, uint64_t flags) {
    if (flags & ~(uint64_t)(O_CLOEXEC | O_NONBLOCK)) return err(LX_EINVAL);
    int32_t id = pipe_alloc();
    if (id < 0) return err(LX_ENOMEM);
    uint32_t st = (uint32_t)flags & O_NONBLOCK;
    int32_t r = of_alloc(OF_PIPE_R, (uint32_t)id, st);
    int32_t w = r < 0 ? -1 : of_alloc(OF_PIPE_W, (uint32_t)id, O_WRONLY | st);
    if (w < 0) {
        if (r >= 0) of_put(r); else pipe_end_closed((uint32_t)id, false);
        if (r < 0) pipe_end_closed((uint32_t)id, true);
        return err(LX_ENOMEM);
    }
    bool cx = (flags & O_CLOEXEC) != 0;
    int64_t fr = fd_install(p, r, 0, cx);
    int64_t fw = fr < 0 ? fr : fd_install(p, w, 0, cx);
    int32_t fds[2] = { (int32_t)fr, (int32_t)fw };
    if (fw < 0 || !uaccess_write(p->address_space, ufds, fds, sizeof(fds))) {
        if (fr >= 0) fd_close(p, fr); else of_put(r);
        if (fw >= 0) fd_close(p, fw); else of_put(w);
        return fw < 0 ? fw : err(LX_EFAULT);
    }
    return 0;
}

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100

/* What a description is ready for now. */
static uint32_t ready_mask(ofile_t* o) {
    switch (o->kind) {
    case OF_CONSOLE:
        return (tty_readable() ? POLLIN | POLLRDNORM : 0) | POLLOUT | POLLWRNORM;
    case OF_PIPE_R: {
        pipe_t* q = &g_pipes[o->ino];
        return (q->len ? POLLIN | POLLRDNORM : 0) | (q->wr ? 0 : POLLHUP);
    }
    case OF_PIPE_W: {
        pipe_t* q = &g_pipes[o->ino];
        return (q->len < PIPE_CAP ? POLLOUT | POLLWRNORM : 0) | (q->rd ? 0 : POLLERR);
    }
    case OF_SOCKET:
        return net_ready((int32_t)o->ino);
    case OF_EPOLL:
        return 0;                                    /* an epoll set in a poll: not ready */
    default:
        return POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
    }
}

/* A wait on it may end by input from outside: the console, or a socket on
 * the network card (#253). */
static bool on_console(ofile_t* o) {
    return o && (o->kind == OF_CONSOLE || (o->kind == OF_SOCKET && net_external((int32_t)o->ino)));
}

/* The wait object of a poll or select: bit 0 when it watches the console,
 * which keeps an idle machine waiting for input. */
#define POLL_CONSOLE 1

/* poll / ppoll. `timeout_ns`: < 0 waits forever. With `retry` the call is
 * the kernel trying again for a blocked process (its deadline stands). */
static int64_t do_poll(process_t* p, uint64_t ufds, uint64_t nfds, int64_t timeout_ns, bool retry) {
    if (nfds > MAX_FDS) return err(LX_EINVAL);
    int64_t ready = 0;
    bool console = false;
    for (uint64_t i = 0; i < nfds; i++) {
        int32_t fd;
        int16_t ev[2];
        if (!uaccess_read(p->address_space, ufds + 8 * i, &fd, 4) ||
            !uaccess_read(p->address_space, ufds + 8 * i + 4, ev, 4)) {
            return err(LX_EFAULT);
        }
        uint16_t rev = 0;
        if (fd >= 0) {
            ofile_t* o = of_get(p, fd);
            if (!o) {
                rev = POLLNVAL;
            } else {
                console |= on_console(o);
                rev = (uint16_t)(ready_mask(o) & ((uint16_t)ev[0] | POLLERR | POLLHUP));
            }
        }
        if (rev) ready++;
        /* revents: written now when ready or not waiting; a wait leaves
         * them zero until the retry that finds something. */
        if (rev || !retry) {
            if (!uaccess_write(p->address_space, ufds + 8 * i + 6, &rev, 2)) return err(LX_EFAULT);
        }
    }
    if (ready || timeout_ns == 0) return ready;
    if (!retry) {
        would_block(WAIT_POLL, console ? POLL_CONSOLE : 0);
        if (timeout_ns > 0) g_block_deadline = clock_monotonic_ns() + (uint64_t)timeout_ns;
    }
    return err(E_BLOCK);
}

/* select / pselect6: the sets are read again on each try and written only
 * when something is ready (or by the timeout). */
static int64_t do_select(process_t* p, int64_t nfds, uint64_t sets[3], int64_t timeout_ns, bool retry) {
    if (nfds < 0 || nfds > MAX_FDS) return err(LX_EINVAL);
    uint32_t bytes = (uint32_t)((nfds + 63) / 64 * 8);
    uint64_t in[3][MAX_FDS / 64], out[3][MAX_FDS / 64];
    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    for (int k = 0; k < 3; k++) {
        if (sets[k] && bytes && !uaccess_read(p->address_space, sets[k], in[k], bytes)) return err(LX_EFAULT);
    }
    static const uint32_t want[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI };
    int64_t ready = 0;
    bool console = false;
    for (int64_t fd = 0; fd < nfds; fd++) {
        uint64_t bit = 1ULL << (fd % 64);
        bool asked = (in[0][fd / 64] | in[1][fd / 64] | in[2][fd / 64]) & bit;
        if (!asked) continue;
        ofile_t* o = of_get(p, fd);
        if (!o) return err(LX_EBADF);
        console |= on_console(o) && (in[0][fd / 64] & bit);
        uint32_t m = ready_mask(o);
        for (int k = 0; k < 3; k++) {
            if ((in[k][fd / 64] & bit) && (m & want[k])) {
                out[k][fd / 64] |= bit;
                ready++;
            }
        }
    }
    if (ready || timeout_ns == 0) {
        for (int k = 0; k < 3; k++) {
            if (sets[k] && bytes && !uaccess_write(p->address_space, sets[k], out[k], bytes)) return err(LX_EFAULT);
        }
        return ready;
    }
    if (!retry) {
        would_block(WAIT_POLL, console ? POLL_CONSOLE : 0);
        if (timeout_ns > 0) g_block_deadline = clock_monotonic_ns() + (uint64_t)timeout_ns;
    }
    return err(E_BLOCK);
}

/* A timeout: struct timespec (ns_scale 1) or struct timeval (1000) at `u`;
 * 0 means none (wait forever: -1). */
static int64_t read_timeout(process_t* p, uint64_t u, uint64_t ns_scale, int64_t* out) {
    if (!u) {
        *out = -1;
        return 0;
    }
    int64_t t[2];
    if (!uaccess_read(p->address_space, u, t, sizeof(t))) return err(LX_EFAULT);
    if (t[0] < 0 || t[1] < 0) return err(LX_EINVAL);
    *out = t[0] * 1000000000LL + t[1] * (int64_t)ns_scale;
    return 0;
}

/* epoll_wait: the interest list's ready entries (level-triggered; EPOLLET
 * is treated as level, EPOLLONESHOT disarms an entry once reported). */
static int64_t do_epoll_wait(process_t* p, int64_t epfd, uint64_t uev, int64_t maxev, int64_t timeout_ns,
                             bool retry) {
    ofile_t* eo = of_get(p, epfd);
    if (!eo || eo->kind != OF_EPOLL) return err(eo ? LX_EINVAL : LX_EBADF);
    if (maxev <= 0) return err(LX_EINVAL);
    epoll_t* e = &g_ep[eo->ino];
    int64_t ready = 0;
    bool outside = false;
    for (uint32_t i = 0; i < e->n && ready < maxev; i++) {
        ep_item_t* it = &e->it[i];
        ofile_t* o = of_get(p, it->fd);
        if (!o) continue;
        outside |= on_console(o);
        uint32_t want = it->events & 0xFFFF;
        uint32_t m = ready_mask(o) & (want | POLLERR | POLLHUP);
        if (!m || !want) continue;
        uint8_t ev[12];
        memcpy(ev, &m, 4);
        memcpy(ev + 4, &it->data, 8);
        if (!uaccess_write(p->address_space, uev + 12 * (uint64_t)ready, ev, 12)) return err(LX_EFAULT);
        if (it->events & (1u << 30)) it->events &= ~0xFFFFu;          /* EPOLLONESHOT */
        ready++;
    }
    if (ready || timeout_ns == 0) return ready;
    if (!retry) {
        would_block(WAIT_POLL, outside ? POLL_CONSOLE : 0);
        if (timeout_ns > 0) g_block_deadline = clock_monotonic_ns() + (uint64_t)timeout_ns;
    }
    return err(E_BLOCK);
}

/* The poll-family calls, by number, from a process's registers. */
static int64_t poll_call(process_t* p, uint64_t nr, bool retry) {
    process_context_t* c = &p->context;
    int64_t to;
    int64_t r;
    switch (nr) {
    case 7:                                                       /* poll */
        to = (int32_t)c->rdx < 0 ? -1 : (int64_t)(int32_t)c->rdx * 1000000LL;
        return do_poll(p, c->rdi, c->rsi, to, retry);
    case 271:                                                     /* ppoll; the mask is not applied */
        if ((r = read_timeout(p, c->rdx, 1, &to)) < 0) return r;
        return do_poll(p, c->rdi, c->rsi, to, retry);
    case 232: case 281:                                           /* epoll_wait, epoll_pwait */
        to = (int32_t)c->r10 < 0 ? -1 : (int64_t)(int32_t)c->r10 * 1000000LL;
        return do_epoll_wait(p, (int64_t)c->rdi, c->rsi, (int64_t)(int32_t)c->rdx, to, retry);
    case 441:                                                     /* epoll_pwait2 */
        if ((r = read_timeout(p, c->r10, 1, &to)) < 0) return r;
        return do_epoll_wait(p, (int64_t)c->rdi, c->rsi, (int64_t)(int32_t)c->rdx, to, retry);
    case 23:                                                      /* select */
    case 270: {                                                   /* pselect6; the mask is not applied */
        if ((r = read_timeout(p, c->r8, nr == 23 ? 1000 : 1, &to)) < 0) return r;
        uint64_t sets[3] = { c->rsi, c->rdx, c->r10 };
        return do_select(p, (int64_t)c->rdi, sets, to, retry);
    }
    default:
        return err(LX_ENOSYS);
    }
}

void files_poll_timed_out(process_t* p) {
    /* select's sets say nothing was ready; poll's revents are zero already. */
    uint64_t nr = p->wait_aux;
    if (nr != 23 && nr != 270) return;
    process_context_t* c = &p->context;
    int64_t nfds = (int64_t)c->rdi;
    if (nfds <= 0 || nfds > MAX_FDS) return;
    uint64_t zero[MAX_FDS / 64];
    memset(zero, 0, sizeof(zero));
    uint64_t sets[3] = { c->rsi, c->rdx, c->r10 };
    for (int k = 0; k < 3; k++) {
        if (sets[k]) uaccess_write(p->address_space, sets[k], zero, (uint32_t)((nfds + 63) / 64 * 8));
    }
}

static bool is_poll_call(uint64_t nr) {
    return nr == 7 || nr == 271 || nr == 23 || nr == 270 || nr == 232 || nr == 281 || nr == 441;
}

/* ---- Socket calls (#253) ---- */

#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000

static ofile_t* sock_of(process_t* p, int64_t fd, int64_t* e) {
    ofile_t* o = of_get(p, fd);
    *e = !o ? err(LX_EBADF) : o->kind != OF_SOCKET ? err(88 /* ENOTSOCK */) : 0;
    return *e ? 0 : o;
}

static int64_t install_socket(process_t* p, int32_t id, uint64_t flags) {
    int32_t of = of_alloc(OF_SOCKET, (uint32_t)id, O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
    if (of < 0) {
        net_close(id);
        return err(LX_ENOMEM);
    }
    int64_t fd = fd_install(p, of, 0, (flags & SOCK_CLOEXEC) != 0);
    if (fd < 0) of_put(of);
    return fd;
}

static int64_t sys_socket(process_t* p, uint64_t domain, uint64_t type, uint64_t proto) {
    (void)proto;
    if (domain != 2) return err(97);                                /* EAFNOSUPPORT: AF_INET only */
    int32_t id = net_socket((int)(type & 0xF));
    if (id < 0) return id;
    return install_socket(p, id, type);
}

static int64_t sys_bind_connect(process_t* p, int64_t fd, uint64_t ua, uint64_t alen, bool connect) {
    int64_t e;
    ofile_t* o = sock_of(p, fd, &e);
    if (!o) return e;
    net_addr_t a;
    if (!get_sockaddr(p, ua, alen, &a, &e)) return e;
    int64_t r = connect ? net_connect((int32_t)o->ino, a, (o->flags & O_NONBLOCK) != 0)
                        : net_bind((int32_t)o->ino, a);
    g_changed |= net_changed();
    if (r == NET_EBLOCK) return sock_wait(o, 0);
    return r;
}

static int64_t sys_accept(process_t* p, int64_t fd, uint64_t ua, uint64_t ualen, uint64_t flags) {
    int64_t e;
    ofile_t* o = sock_of(p, fd, &e);
    if (!o) return e;
    int32_t ns;
    net_addr_t peer;
    int64_t r = net_accept((int32_t)o->ino, &ns, &peer);
    g_changed |= net_changed();
    if (r == NET_EBLOCK) return sock_wait(o, 0);
    if (r < 0) return r;
    int64_t nfd = install_socket(p, ns, flags);
    if (nfd < 0) return nfd;
    e = put_sockaddr(p, ua, ualen, peer);
    return e < 0 ? e : nfd;
}

/* sendmsg / recvmsg: the iovecs through the same paths, one at a time. */
static int64_t sys_msg(process_t* p, int64_t fd, uint64_t umsg, uint64_t flags, bool send) {
    int64_t e;
    ofile_t* o = sock_of(p, fd, &e);
    if (!o) return e;
    uint64_t m[7];                                                  /* struct msghdr */
    if (!uaccess_read(p->address_space, umsg, m, sizeof(m))) return err(LX_EFAULT);
    uint64_t name = m[0], namelen = (uint32_t)m[1], iov = m[2], iovlen = m[3];
    if (iovlen > 1024) return err(LX_EINVAL);
    int64_t total = 0;
    for (uint64_t i = 0; i < iovlen; i++) {
        uint64_t v[2];
        if (!uaccess_read(p->address_space, iov + 16 * i, v, 16)) return total ? total : err(LX_EFAULT);
        if (!v[1]) continue;
        int64_t n = send ? sock_send(p, o, v[0], v[1], (uint32_t)flags, total ? 0 : name, namelen)
                         : sock_recv(p, o, v[0], v[1], (uint32_t)flags, total ? 0 : name,
                                     total || !name ? 0 : umsg + 8);
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint64_t)n < v[1] || (!send && net_type((int32_t)o->ino) == NET_SOCK_DGRAM)) break;
    }
    if (!send) {
        uint64_t zero = 0;
        uaccess_write(p->address_space, umsg + 40, &zero, 8);          /* msg_controllen */
        uaccess_write(p->address_space, umsg + 48, &zero, 4);          /* msg_flags */
    }
    return total;
}

static int64_t sys_sockopt(process_t* p, int64_t fd, uint64_t level, uint64_t opt, uint64_t uval,
                           uint64_t ulen, bool get) {
    int64_t e;
    ofile_t* o = sock_of(p, fd, &e);
    if (!o) return e;
    if (!get) return 0;                                             /* accepted, not applied */
    int32_t v = 0;
    if (level == 1) {                                               /* SOL_SOCKET */
        if (opt == 4) v = net_take_error((int32_t)o->ino);          /* SO_ERROR */
        else if (opt == 3) v = net_type((int32_t)o->ino);           /* SO_TYPE */
        else if (opt == 7 || opt == 8) v = 65536;                   /* SO_SNDBUF, SO_RCVBUF */
        else if (opt == 39) v = 2;                                  /* SO_DOMAIN */
    }
    uint32_t len = 4;
    if (!uaccess_write(p->address_space, uval, &v, 4) || !uaccess_write(p->address_space, ulen, &len, 4)) {
        return err(LX_EFAULT);
    }
    return 0;
}

static int64_t sys_sockname(process_t* p, int64_t fd, uint64_t ua, uint64_t ualen, bool peer) {
    int64_t e;
    ofile_t* o = sock_of(p, fd, &e);
    if (!o) return e;
    net_addr_t a;
    int64_t r = net_name((int32_t)o->ino, peer, &a);
    return r < 0 ? r : put_sockaddr(p, ua, ualen, a);
}

static int64_t sys_epoll_create(process_t* p, uint64_t flags) {
    int32_t id = epoll_alloc();
    if (id < 0) return err(LX_ENOMEM);
    int32_t of = of_alloc(OF_EPOLL, (uint32_t)id, O_RDWR);
    if (of < 0) {
        g_ep[id].used = false;
        return err(LX_ENOMEM);
    }
    int64_t fd = fd_install(p, of, 0, (flags & O_CLOEXEC) != 0);
    if (fd < 0) of_put(of);
    return fd;
}

static int64_t sys_epoll_ctl(process_t* p, int64_t epfd, uint64_t op, int64_t fd, uint64_t uev) {
    ofile_t* eo = of_get(p, epfd);
    if (!eo || eo->kind != OF_EPOLL) return err(eo ? LX_EINVAL : LX_EBADF);
    if (!of_get(p, fd) || fd == epfd) return err(LX_EBADF);
    epoll_t* e = &g_ep[eo->ino];
    uint32_t i = 0;
    while (i < e->n && e->it[i].fd != fd) i++;
    uint8_t ev[12];
    if (op != 2 && !uaccess_read(p->address_space, uev, ev, 12)) return err(LX_EFAULT);
    switch (op) {
    case 1:                                                         /* EPOLL_CTL_ADD */
        if (i < e->n) return err(LX_EEXIST);
        if (e->n == EP_MAX) return err(LX_ENOMEM);
        e->n++;
        /* fall through */
    case 3:                                                         /* EPOLL_CTL_MOD */
        if (i == e->n) return err(LX_ENOENT);
        e->it[i].fd = (int32_t)fd;
        memcpy(&e->it[i].events, ev, 4);
        memcpy(&e->it[i].data, ev + 4, 8);
        return 0;
    case 2:                                                         /* EPOLL_CTL_DEL */
        if (i == e->n) return err(LX_ENOENT);
        memmove(&e->it[i], &e->it[i + 1], (e->n - i - 1) * sizeof(ep_item_t));
        e->n--;
        return 0;
    default:
        return err(LX_EINVAL);
    }
}

/* ---- Blocked calls, retried (#249) ---- */

static int64_t dispatch(process_t* p, uint64_t nr, bool* handled);

static bool retryable(const process_t* p) {
    if (p->state != PROCESS_STATE_BLOCKED) return false;
    return p->wait_reason == WAIT_INPUT || p->wait_reason == WAIT_PIPE || p->wait_reason == WAIT_POLL ||
           p->wait_reason == WAIT_NET;
}

/* A pipe or the console changed: every blocked read, write, and poll tries
 * again, in process-table order, until a pass wakes nobody. Deterministic:
 * the changes happen at journaled or replayed steps. */
void files_retry_waiters(void) {
    for (uint32_t pass = 0; pass < 64; pass++) {
        g_changed = false;
        bool woke = false;
        for (uint32_t i = 0; i < proc_count(); i++) {
            process_t* p = proc_at(i);
            if (!retryable(p) || !fdt(p)) continue;
            uint64_t nr = p->wait_aux;
            int64_t r;
            if (is_poll_call(nr)) {
                r = poll_call(p, nr, true);
            } else {
                bool handled;
                r = dispatch(p, nr, &handled);
            }
            if (r == err(E_BLOCK)) continue;
            machine_wake(p, r);
            woke = true;
        }
        g_changed |= net_changed();
        if (!woke && !g_changed) return;
    }
}

bool files_input_waited(void) {
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* p = proc_at(i);
        if (p->state != PROCESS_STATE_BLOCKED) continue;
        if (p->wait_reason == WAIT_INPUT || (p->wait_reason == WAIT_POLL && (p->wait_obj & POLL_CONSOLE)) ||
            (p->wait_reason == WAIT_NET && (p->wait_obj >> 32))) {
            return true;
        }
    }
    return false;
}

void files_console_signal(uint32_t sig) {
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* p = proc_at(i);
        fdt_t* t = fdt(p);
        if (!t || p->pid != (pid_t)p->tgid || p->state == PROCESS_STATE_ZOMBIE) continue;
        for (uint32_t fd = 0; fd < MAX_FDS; fd++) {
            if (t->of[fd] >= 0 && g_of[t->of[fd]].kind == OF_CONSOLE) {
                ksig_send(p, sig, 0x80, 0);                       /* SI_KERNEL */
                break;
            }
        }
    }
}

/* One call from p's registers; *handled false if `nr` is not a file call. */
static int64_t dispatch(process_t* p, uint64_t nr, bool* handled) {
    process_context_t* c = &p->context;
    uint64_t a0 = c->rdi, a1 = c->rsi, a2 = c->rdx, a3 = c->r10, a4 = c->r8, a5 = c->r9;
    int64_t r;
    *handled = true;
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
        if (a3 & MAP_ANONYMOUS) {
            *handled = false;
            return 0;
        }
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
    case 41:  r = sys_socket(p, a0, a1, a2); break;
    case 42:  r = sys_bind_connect(p, (int64_t)a0, a1, a2, true); break;          /* connect */
    case 49:  r = sys_bind_connect(p, (int64_t)a0, a1, a2, false); break;         /* bind */
    case 50: {                                                                   /* listen */
        int64_t e;
        ofile_t* o = sock_of(p, (int64_t)a0, &e);
        r = o ? net_listen((int32_t)o->ino, (int)a1) : e;
        break;
    }
    case 43:  r = sys_accept(p, (int64_t)a0, a1, a2, 0); break;
    case 288: r = sys_accept(p, (int64_t)a0, a1, a2, a3); break;                 /* accept4 */
    case 44: {                                                                   /* sendto */
        int64_t e;
        ofile_t* o = sock_of(p, (int64_t)a0, &e);
        r = o ? sock_send(p, o, a1, a2, (uint32_t)a3, a4, a5) : e;
        break;
    }
    case 45: {                                                                   /* recvfrom */
        int64_t e;
        ofile_t* o = sock_of(p, (int64_t)a0, &e);
        r = o ? sock_recv(p, o, a1, a2, (uint32_t)a3, a4, a5) : e;
        break;
    }
    case 46:  r = sys_msg(p, (int64_t)a0, a1, a2, true); break;                  /* sendmsg */
    case 47:  r = sys_msg(p, (int64_t)a0, a1, a2, false); break;                 /* recvmsg */
    case 48: {                                                                   /* shutdown */
        int64_t e;
        ofile_t* o = sock_of(p, (int64_t)a0, &e);
        r = o ? net_shutdown((int32_t)o->ino, (int)a1) : e;
        g_changed |= net_changed();
        break;
    }
    case 51:  r = sys_sockname(p, (int64_t)a0, a1, a2, false); break;            /* getsockname */
    case 52:  r = sys_sockname(p, (int64_t)a0, a1, a2, true); break;             /* getpeername */
    case 53:  r = err(97); break;                                                /* socketpair: AF_UNIX */
    case 54:  r = sys_sockopt(p, (int64_t)a0, a1, a2, a3, a4, false); break;     /* setsockopt */
    case 55:  r = sys_sockopt(p, (int64_t)a0, a1, a2, a3, a4, true); break;      /* getsockopt */
    case 213: r = (int64_t)a0 <= 0 ? err(LX_EINVAL) : sys_epoll_create(p, 0); break;
    case 291: r = sys_epoll_create(p, a0); break;                                /* epoll_create1 */
    case 233: r = sys_epoll_ctl(p, (int64_t)a0, a1, (int64_t)a2, a3); break;
    case 22:  r = sys_pipe2(p, a0, 0); break;                                    /* pipe */
    case 293: r = sys_pipe2(p, a0, a1); break;                                   /* pipe2 */
    default:
        *handled = false;
        return 0;
    }
    return r;
}

bool files_syscall(process_t* p, uint64_t nr, int64_t* ret) {
    if (!fdt(p)) return false;
    bool handled = true;
    int64_t r = is_poll_call(nr) ? poll_call(p, nr, false)
                                                                 : dispatch(p, nr, &handled);
    if (!handled) return false;
    if (r == err(E_BLOCK)) {
        machine_block(p, g_block_reason, g_block_obj, g_block_deadline);
        p->wait_aux = nr;                 /* what files_retry_waiters runs again */
        *ret = FILES_SWITCHED;
    } else {
        *ret = r;
    }
    if (g_changed) files_retry_waiters();
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
        /* v2: a table shared with an earlier process (a thread, #251) is
         * written once and named by that process's pid. */
        uint32_t first = i;
        for (uint32_t k = 0; t && k < i; k++) {
            if (fdt(proc_at(k)) == t) { first = k; break; }
        }
        if (t && first != i) {
            ksf_put_u32(w, 2);
            ksf_put_u32(w, (uint32_t)proc_at(first)->pid);
            continue;
        }
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
    /* v3: pipes and their buffered bytes, oldest first (#249). */
    uint32_t npipes = 0;
    for (uint32_t i = 0; i < g_npipes; i++) npipes += g_pipes[i].used;
    ksf_put_u32(w, npipes);
    for (uint32_t i = 0; i < g_npipes; i++) {
        pipe_t* q = &g_pipes[i];
        if (!q->used) continue;
        ksf_put_u32(w, i);
        ksf_put_u32(w, (uint32_t)q->rd | (uint32_t)q->wr << 1);
        ksf_put_u32(w, q->len);
        uint32_t first = PIPE_CAP - q->head < q->len ? PIPE_CAP - q->head : q->len;
        ksf_put(w, q->buf + q->head, first);
        ksf_put(w, q->buf, q->len - first);
    }
    /* v4: epoll sets (#253). */
    uint32_t neps = 0;
    for (uint32_t i = 0; i < g_nep; i++) neps += g_ep[i].used;
    ksf_put_u32(w, neps);
    for (uint32_t i = 0; i < g_nep; i++) {
        if (!g_ep[i].used) continue;
        ksf_put_u32(w, i);
        ksf_put_u32(w, g_ep[i].n);
        ksf_put(w, g_ep[i].it, g_ep[i].n * sizeof(ep_item_t));
    }
}

static int epolls_restore(ksf_reader_t* r) {
    if (g_ep) memset(g_ep, 0, (uint64_t)g_nep * sizeof(epoll_t));
    uint32_t n = ksf_get_u32(r);
    for (uint32_t k = 0; k < n && r->ok; k++) {
        uint32_t id = ksf_get_u32(r);
        if (id > 4096) return -1;
        while (id >= g_nep) {
            uint32_t cap = g_nep ? g_nep * 2 : 8;
            epoll_t* nn = (epoll_t*)kzalloc((uint64_t)cap * sizeof(epoll_t));
            if (!nn) return -1;
            if (g_ep) {
                memcpy(nn, g_ep, (uint64_t)g_nep * sizeof(epoll_t));
                kfree(g_ep);
            }
            g_ep = nn;
            g_nep = cap;
        }
        epoll_t* e = &g_ep[id];
        e->used = true;
        e->n = ksf_get_u32(r);
        if (e->n > EP_MAX) return -1;
        ksf_get(r, e->it, e->n * sizeof(ep_item_t));
    }
    return r->ok ? 0 : -1;
}

static void pipes_clear(void) {
    for (uint32_t i = 0; i < g_npipes; i++) {
        if (g_pipes[i].used) kfree(g_pipes[i].buf);
        memset(&g_pipes[i], 0, sizeof(pipe_t));
    }
}

static int pipes_restore(ksf_reader_t* r) {
    uint32_t n = ksf_get_u32(r);
    for (uint32_t k = 0; k < n && r->ok; k++) {
        uint32_t id = ksf_get_u32(r);
        if (id > (1u << 16)) return -1;
        while (id >= g_npipes) {
            uint32_t cap = g_npipes ? g_npipes * 2 : 16;
            pipe_t* nn = (pipe_t*)kzalloc((uint64_t)cap * sizeof(pipe_t));
            if (!nn) return -1;
            if (g_pipes) {
                memcpy(nn, g_pipes, (uint64_t)g_npipes * sizeof(pipe_t));
                kfree(g_pipes);
            }
            g_pipes = nn;
            g_npipes = cap;
        }
        pipe_t* q = &g_pipes[id];
        uint32_t ends = ksf_get_u32(r);
        q->len = ksf_get_u32(r);
        if (q->len > PIPE_CAP) return -1;
        q->buf = (uint8_t*)kmalloc(PIPE_CAP);
        if (!q->buf) return -1;
        q->used = true;
        q->rd = (ends & 1) != 0;
        q->wr = (ends & 2) != 0;
        q->head = 0;
        ksf_get(r, q->buf, q->len);
    }
    return r->ok ? 0 : -1;
}

static int files_restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    if (version < 1 || version > 4) return -1;
    pipes_clear();
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
        uint32_t has = ksf_get_u32(r);
        if (!p) return -1;
        if (has == 2) {
            process_t* o = proc_by_pid(ksf_get_u32(r));
            if (!o || !fdt(o)) return -1;
            fdt(o)->users++;
            p->files = o->files;
            continue;
        }
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
    if (version >= 3 && pipes_restore(r) != 0) return -1;
    if (version >= 4 && epolls_restore(r) != 0) return -1;
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
        bool again = false;
        for (uint32_t k = 0; k < i; k++) again |= fdt(proc_at(k)) == t;
        if (again) {
            uint32_t head[2] = { (uint32_t)proc_at(i)->pid, t->users };
            crc = divergence_checksum(crc, head, sizeof(head));
            continue;
        }
        uint32_t head[3] = { (uint32_t)proc_at(i)->pid, t->cwd, t->umask };
        crc = divergence_checksum(crc, head, sizeof(head));
        for (uint32_t fd = 0; fd < MAX_FDS; fd++) {
            if (t->of[fd] < 0) continue;
            uint32_t e[3] = { fd, (uint32_t)t->of[fd], t->cloexec[fd] };
            crc = divergence_checksum(crc, e, sizeof(e));
        }
    }
    for (uint32_t i = 0; i < g_npipes; i++) {
        pipe_t* q = &g_pipes[i];
        if (!q->used) continue;
        uint32_t head[3] = { i, (uint32_t)q->rd | (uint32_t)q->wr << 1, q->len };
        crc = divergence_checksum(crc, head, sizeof(head));
        uint32_t first = PIPE_CAP - q->head < q->len ? PIPE_CAP - q->head : q->len;
        crc = divergence_checksum(crc, q->buf + q->head, first);
        crc = divergence_checksum(crc, q->buf, q->len - first);
    }
    for (uint32_t i = 0; i < g_nep; i++) {
        if (!g_ep[i].used) continue;
        crc = divergence_checksum(crc, &i, sizeof(i));
        crc = divergence_checksum(crc, g_ep[i].it, g_ep[i].n * sizeof(ep_item_t));
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
    static const ksf_subsys_t files = { KS_ID_FILES, "files", 4, files_save, files_restore, files_sum, 0 };
    if (kstate_register(&fs) != KSF_OK || kstate_register(&files) != KSF_OK) {
        panic("files: cannot register keyframe sections");
    }
    static const kfp_provider_t prov = { FS_PID_BASE, fs_pages, fs_attach };
    kfp_set_provider(&prov);
}
