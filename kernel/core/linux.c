/* Laplace core: the Linux x86-64 system call interface (#238).
 * See include/core/linux.h. */

#include "core/linux.h"
#include "core/machine.h"
#include "core/proc.h"
#include "core/mm.h"
#include "core/clock.h"
#include "core/vm.h"
#include "core/console.h"
#include "entropy_record.h"
#include "time_record.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

#define TCGETS      0x5401
#define TIOCGWINSZ  0x5413

#define RLIMIT_STACK  3
#define RLIMIT_NOFILE 7
#define RLIM_INFINITY (~0ULL)

static inline int64_t err(int e) { return -(int64_t)e; }

static bool put_user(process_t* p, uint64_t uaddr, const void* src, uint64_t len) {
    return uaccess_write(p->address_space, uaddr, src, len);
}

/* ---- Console output (fds 1 and 2) ---- */

static int64_t write_console(process_t* p, uint64_t buf, uint64_t len) {
    char tmp[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
        if (!uaccess_read(p->address_space, buf + done, tmp, n)) {
            return done ? (int64_t)done : err(LX_EFAULT);
        }
        if (!machine_quiet()) console_write(tmp, (uint32_t)n);
        done += n;
    }
    return (int64_t)done;
}

static int64_t sys_write(process_t* p, int64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 1 && fd != 2) return err(LX_EBADF);
    return write_console(p, buf, len);
}

static int64_t sys_writev(process_t* p, int64_t fd, uint64_t iov, uint64_t cnt) {
    if (fd != 1 && fd != 2) return err(LX_EBADF);
    if (cnt > 1024) return err(LX_EINVAL);
    int64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t v[2];
        if (!uaccess_read(p->address_space, iov + 16 * i, v, sizeof(v))) return err(LX_EFAULT);
        int64_t n = write_console(p, v[0], v[1]);
        if (n < 0) return total ? total : n;
        total += n;
    }
    return total;
}

/* ---- Identity and limits ---- */

static int64_t sys_uname(process_t* p, uint64_t buf) {
    static const char* fields[6] = { "Linux", "laplace", "6.1.0-laplace", "#1 Laplace",
                                     "x86_64", "(none)" };
    char u[6 * 65];
    memset(u, 0, sizeof(u));
    for (int f = 0; f < 6; f++) {
        for (int i = 0; fields[f][i] && i < 64; i++) u[f * 65 + i] = fields[f][i];
    }
    return put_user(p, buf, u, sizeof(u)) ? 0 : err(LX_EFAULT);
}

static int64_t sys_prlimit(process_t* p, uint64_t resource, uint64_t old) {
    if (!old) return 0;
    uint64_t lim[2] = { RLIM_INFINITY, RLIM_INFINITY };
    if (resource == RLIMIT_STACK) lim[0] = lim[1] = 8ULL << 20;
    if (resource == RLIMIT_NOFILE) lim[0] = lim[1] = 1024;
    return put_user(p, old, lim, sizeof(lim)) ? 0 : err(LX_EFAULT);
}

/* ---- Thread-local storage ---- */

static int64_t sys_arch_prctl(process_t* p, uint64_t code, uint64_t addr) {
    lp_arch_t* a = proc_arch(p);
    switch (code) {
    case ARCH_SET_FS:
        if (addr >= LP_USER_END) return err(LX_EPERM);
        a->fs_base = addr;
        machine_load_segment_bases(p);
        return 0;
    case ARCH_SET_GS:
        if (addr >= LP_USER_END) return err(LX_EPERM);
        a->gs_base = addr;
        machine_load_segment_bases(p);
        return 0;
    case ARCH_GET_FS:
        return put_user(p, addr, &a->fs_base, 8) ? 0 : err(LX_EFAULT);
    case ARCH_GET_GS:
        return put_user(p, addr, &a->gs_base, 8) ? 0 : err(LX_EFAULT);
    default:
        return err(LX_EINVAL);
    }
}

/* ---- Time and randomness (journaled) ---- */

static int64_t sys_clock_gettime(process_t* p, uint64_t clk, uint64_t ts) {
    uint64_t ns;
    switch (clk) {
    case 0: case 5: case 8:       /* REALTIME, REALTIME_COARSE, REALTIME_ALARM */
        ns = clock_realtime_ns();
        break;
    case 1: case 4: case 6: case 7: case 9:   /* MONOTONIC*, BOOTTIME* */
    case 2: case 3:               /* PROCESS/THREAD_CPUTIME: one CPU, close enough */
        ns = clock_monotonic_ns();
        break;
    default:
        return err(LX_EINVAL);
    }
    uint64_t t[2] = { ns / 1000000000ULL, ns % 1000000000ULL };
    return put_user(p, ts, t, sizeof(t)) ? 0 : err(LX_EFAULT);
}

static int64_t sys_gettimeofday(process_t* p, uint64_t tv, uint64_t tz) {
    if (tv) {
        uint64_t ns = clock_realtime_ns();
        uint64_t t[2] = { ns / 1000000000ULL, (ns % 1000000000ULL) / 1000 };
        if (!put_user(p, tv, t, sizeof(t))) return err(LX_EFAULT);
    }
    if (tz) {
        uint32_t z[2] = { 0, 0 };
        if (!put_user(p, tz, z, sizeof(z))) return err(LX_EFAULT);
    }
    return 0;
}

static int64_t sys_time(process_t* p, uint64_t tloc) {
    int64_t s = (int64_t)(clock_realtime_ns() / 1000000000ULL);
    if (tloc && !put_user(p, tloc, &s, 8)) return err(LX_EFAULT);
    return s;
}

static int64_t sys_getrandom(process_t* p, uint64_t buf, uint64_t len) {
    uint8_t tmp[256];
    /* Short reads are allowed; keep each call within the entropy journal. */
    if (len > sizeof(tmp)) len = sizeof(tmp);
    if (kentropy_fill(tmp, (uint32_t)len) != ENTROPY_REC_OK) return err(LX_EAGAIN);
    return put_user(p, buf, tmp, len) ? (int64_t)len : err(LX_EFAULT);
}

/* ---- Signals (no delivery yet, #250) ---- */

static int64_t sys_rt_sigaction(process_t* p, uint64_t oact) {
    if (oact) {
        uint8_t z[32];
        memset(z, 0, sizeof(z));
        if (!put_user(p, oact, z, sizeof(z))) return err(LX_EFAULT);
    }
    return 0;
}

static int64_t sys_rt_sigprocmask(process_t* p, uint64_t oset) {
    if (oset) {
        uint64_t z = 0;
        if (!put_user(p, oset, &z, 8)) return err(LX_EFAULT);
    }
    return 0;
}

/* ---- Unimplemented calls ---- */

static uint8_t g_reported[512 / 8];

static int64_t enosys(process_t* p, uint64_t nr) {
    if (nr < 512 && !(g_reported[nr / 8] & (1u << (nr % 8)))) {
        g_reported[nr / 8] |= (uint8_t)(1u << (nr % 8));
        if (!machine_quiet()) {
            kprintf("linux: pid %d: system call %lu is not implemented (rip %lx)\n",
                    p->pid, nr, p->context.rip);
        }
    }
    return err(LX_ENOSYS);
}

bool linux_is_exit(uint64_t nr) {
    return nr == LX_exit || nr == LX_exit_group;
}

lx_after_t linux_syscall(process_t* p) {
    process_context_t* c = &p->context;
    uint64_t nr = c->rax;
    uint64_t a0 = c->rdi, a1 = c->rsi, a2 = c->rdx, a3 = c->r10;
    int64_t r;

    switch (nr) {
    case LX_exit:
    case LX_exit_group:
        machine_terminate(p, (int)a0);
        return LX_SWITCHED;
    case LX_sched_yield:
        c->rax = 0;
        machine_yield(p);
        return LX_SWITCHED;
    case LX_write:           r = sys_write(p, (int64_t)a0, a1, a2); break;
    case LX_writev:          r = sys_writev(p, (int64_t)a0, a1, a2); break;
    case LX_read:            r = a0 == 0 ? 0 : err(LX_EBADF); break;   /* stdin: #249 */
    case LX_close:           r = a0 <= 2 ? 0 : err(LX_EBADF); break;
    case LX_ioctl:
        r = (a1 == TIOCGWINSZ || a1 == TCGETS) ? err(LX_ENOTTY) : err(LX_EINVAL);
        if ((int64_t)a0 < 0 || a0 > 2) r = err(LX_EBADF);
        break;
    case LX_getpid:
    case LX_gettid:          r = p->pid; break;
    case LX_getppid:         r = p->ppid; break;
    case LX_getuid: case LX_geteuid: case LX_getgid: case LX_getegid:
                             r = 0; break;
    case LX_uname:           r = sys_uname(p, a0); break;
    case LX_arch_prctl:      r = sys_arch_prctl(p, a0, a1); break;
    case LX_set_tid_address: proc_arch(p)->clear_child_tid = a0; r = p->pid; break;
    case LX_set_robust_list: r = 0; break;
    case LX_rseq:            r = err(LX_ENOSYS); break;   /* libc falls back quietly */
    case LX_prlimit64:       r = sys_prlimit(p, a1, a3); break;
    case LX_getrlimit:       r = sys_prlimit(p, a0, a1); break;
    case LX_clock_gettime:   r = sys_clock_gettime(p, a0, a1); break;
    case LX_gettimeofday:    r = sys_gettimeofday(p, a0, a1); break;
    case LX_time:            r = sys_time(p, a0); break;
    case LX_getrandom:       r = sys_getrandom(p, a0, a1); break;
    case LX_rt_sigaction:    r = sys_rt_sigaction(p, a2); break;
    case LX_rt_sigprocmask:  r = sys_rt_sigprocmask(p, a2); break;
    case LX_sigaltstack:     r = 0; break;
    case LX_brk:             r = vm_brk(p, a0); break;
    case LX_mmap:            r = vm_mmap(p, a0, a1, a2, a3, (int64_t)c->r8, c->r9); break;
    case LX_munmap:          r = vm_munmap(p, a0, a1); break;
    case LX_mprotect:        r = vm_mprotect(p, a0, a1, a2); break;
    case LX_mremap:          r = vm_mremap(p, a0, a1, a2, a3, c->r8); break;
    case LX_madvise:         r = vm_madvise(p, a0, a1, a2); break;
    default:                 r = enosys(p, nr); break;
    }
    c->rax = (uint64_t)r;
    return LX_CONTINUE;
}
