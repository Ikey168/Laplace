/* Laplace core: processes that start processes (#248): fork, execve, wait,
 * exit, kill. See include/core/pctl.h. */

#include "core/pctl.h"
#include "core/linux.h"
#include "core/machine.h"
#include "core/proc.h"
#include "core/mm.h"
#include "core/vm.h"
#include "core/files.h"
#include "core/timetravel.h"
#include "core/console.h"
#include "core/ksig.h"
#include "core/clock.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

static inline int64_t err(int e) { return -(int64_t)e; }

#define WNOHANG      1
#define SIGCHLD      17
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_VFORK          0x00004000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* ---- fork ---- */

typedef struct {
    vm_space_t* child;
    bool        current;   /* the parent's space is loaded */
    bool        ok;
} fork_ctx_t;

/* Share one page: both spaces map the frame read-only, tagged for fork COW;
 * the first write in either copies it (vmm_user_write_fault). */
static void share_page(void* c, uint64_t va, pte_t* pte) {
    fork_ctx_t* f = (fork_ctx_t*)c;
    uint64_t frame = *pte & LP_PTE_ADDR_MASK;
    uint32_t user = (*pte & PAGE_USER) ? PAGE_USER : 0;
    frame_share(frame);
    if (vmm_map_page(f->child, va, frame, user | PAGE_FORK_COW) != 0) {
        frame_put(frame);
        f->ok = false;
        return;
    }
    *pte = (*pte & ~(pte_t)PAGE_WRITABLE) | PAGE_FORK_COW;
    if (f->current) vmm_flush_tlb_page(va);
}

static process_t* do_fork(process_t* p, int64_t* err_out) {
    process_t* c = proc_alloc(proc_take_pid(), p->name);
    if (!c) {
        *err_out = err(LX_ENOMEM);
        return 0;
    }
    for (vm_region_t* r = p->address_space->regions; r; r = r->next) {
        if (!vmm_create_region(c->address_space, r->start_addr, r->end_addr - r->start_addr, r->flags,
                               r->type, r->name)) {
            proc_destroy(c);
            *err_out = err(LX_ENOMEM);
            return 0;
        }
    }
    fork_ctx_t f = { c->address_space, vmm_get_current_space() == p->address_space, true };
    vmm_for_each_pte(p->address_space, share_page, &f);
    if (!f.ok) {
        proc_destroy(c);
        *err_out = err(LX_ENOMEM);
        return 0;
    }
    machine_fpu_flush();                       /* the parent's live FPU registers */
    c->context = p->context;
    c->context.rax = 0;                        /* fork returns 0 in the child */
    memcpy(c->arch, p->arch, sizeof(lp_arch_t));
    c->ppid = (pid_t)p->tgid;                  /* a thread's children are its group's */
    c->entry_point = p->entry_point;
    c->stack_start = p->stack_start;
    c->stack_end = p->stack_end;
    memcpy(c->exe_path, p->exe_path, sizeof(c->exe_path));
    c->exe_crc = p->exe_crc;
    files_fork(p, c);
    ksig_fork(p, c);
    c->state = PROCESS_STATE_READY;
    proc_append(c);
    return c;
}

/* A thread (#251): a new scheduling unit in p's group, with its own
 * registers, extended state, FS base, and signal mask, sharing the address
 * space, descriptors, and handlers. */
static process_t* do_thread(process_t* p, int64_t* err_out) {
    process_t* c = proc_alloc(proc_take_pid(), p->name);
    if (!c) {
        *err_out = err(LX_ENOMEM);
        return 0;
    }
    vmm_destroy_address_space(c->address_space);
    c->address_space = p->address_space;
    machine_fpu_flush();
    c->context = p->context;
    c->context.rax = 0;
    memcpy(c->arch, p->arch, sizeof(lp_arch_t));
    proc_arch(c)->clear_child_tid = 0;
    c->tgid = p->tgid;
    c->ppid = p->ppid;
    c->entry_point = p->entry_point;
    c->stack_start = p->stack_start;
    c->stack_end = p->stack_end;
    memcpy(c->exe_path, p->exe_path, sizeof(c->exe_path));
    c->exe_crc = p->exe_crc;
    files_share(p, c);
    ksig_thread(p, c);
    c->state = PROCESS_STATE_READY;
    proc_append(c);
    return c;
}

static int64_t sys_clone(process_t* p, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t ctid,
                         uint64_t tls) {
    uint64_t thread = CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
    if ((flags & CLONE_VM) && (flags & thread) != thread) return err(LX_EINVAL);   /* vfork-like sharing */
    int64_t e = 0;
    process_t* c = (flags & CLONE_THREAD) ? do_thread(p, &e) : do_fork(p, &e);
    if (!c) return e;
    if (newsp) c->context.rsp = newsp;
    if (flags & CLONE_SETTLS) proc_arch(c)->fs_base = tls;
    if (flags & CLONE_CHILD_CLEARTID) proc_arch(c)->clear_child_tid = ctid;
    uint32_t tid = (uint32_t)c->pid;
    if (flags & CLONE_CHILD_SETTID) uaccess_write(c->address_space, ctid, &tid, 4);
    if (flags & CLONE_PARENT_SETTID) uaccess_write(p->address_space, ptid, &tid, 4);
    return c->pid;
}

/* ---- execve ---- */

#define MAX_ARGS   256
#define ARG_BYTES  (128 * 1024)

/* Copy a NULL-terminated array of user strings into `pool`. */
static int64_t copy_strings(process_t* p, uint64_t uarr, const char** out, uint32_t* n, char* pool,
                            uint32_t* used) {
    *n = 0;
    if (!uarr) return 0;
    for (;;) {
        uint64_t ptr;
        if (!uaccess_read(p->address_space, uarr + 8ULL * *n, &ptr, 8)) return err(LX_EFAULT);
        if (!ptr) return 0;
        if (*n >= MAX_ARGS) return err(7 /* E2BIG */);
        out[*n] = pool + *used;
        for (;;) {
            if (*used >= ARG_BYTES) return err(7);
            char ch;
            if (!uaccess_read(p->address_space, ptr++, &ch, 1)) return err(LX_EFAULT);
            pool[(*used)++] = ch;
            if (!ch) break;
        }
        (*n)++;
    }
}

static int64_t sys_execve(process_t* p, uint64_t upath, uint64_t uargv, uint64_t uenvp) {
    static char pool[ARG_BYTES];
    static const char* argv[MAX_ARGS];
    static const char* envp[MAX_ARGS];
    static char path[256];
    uint32_t used = 0, argc = 0, envc = 0;
    for (uint32_t i = 0; i < sizeof(path); i++) {
        if (!uaccess_read(p->address_space, upath + i, &path[i], 1)) return err(LX_EFAULT);
        if (!path[i]) break;
        if (i == sizeof(path) - 1) return err(36 /* ENAMETOOLONG */);
    }
    int64_t rc = copy_strings(p, uargv, argv, &argc, pool, &used);
    if (rc < 0) return rc;
    if ((rc = copy_strings(p, uenvp, envp, &envc, pool, &used)) < 0) return rc;
    const lp_image_t* img = proc_find_image(path);
    if (!img) return err(LX_ENOENT);
    if (!img->linux_abi) return err(8 /* ENOEXEC */);
    /* The other threads go (#251); a thread that is not the leader takes
     * the group's pid, as on Linux. */
    for (uint32_t i = 0; i < proc_count();) {
        process_t* t = proc_at(i);
        if (t->tgid == p->tgid && t != p) {
            proc_destroy(t);
            continue;
        }
        i++;
    }
    p->pid = (pid_t)p->tgid;
    rc = proc_exec(p, img, argc, argv, envc, envp);
    if (rc < 0) return rc;
    files_exec(p);                              /* close-on-exec descriptors */
    ksig_exec(p);                               /* handlers back to default */
    return 0;
}

/* ---- exit and wait ---- */

static bool child_matches(const process_t* parent, const process_t* c, int64_t pid) {
    if (c->ppid != (pid_t)parent->tgid || c->pid != (pid_t)c->tgid) return false;   /* leaders only */
    return pid <= 0 || c->pid == pid;          /* process groups: any child */
}

static int64_t reap(process_t* parent, process_t* c, uint64_t ustatus) {
    int32_t status = (int32_t)c->wait_status;
    if (ustatus && !uaccess_write(parent->address_space, ustatus, &status, 4)) return err(LX_EFAULT);
    int64_t pid = c->pid;
    proc_destroy(c);
    return pid;
}

void pctl_child_exited(process_t* c) {
    /* Any thread of the parent's group may be the one waiting. */
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* w = proc_at(i);
        if (w->tgid != (uint32_t)c->ppid || w->state != PROCESS_STATE_BLOCKED || w->wait_reason != WAIT_CHILD) {
            continue;
        }
        if (!child_matches(w, c, (int64_t)w->wait_obj)) continue;
        int64_t r = reap(w, c, w->wait_aux);
        machine_wake(w, r);
        return;
    }
}

static lx_after_t sys_wait4(process_t* p, int64_t pid, uint64_t ustatus, uint64_t options,
                            uint64_t rusage, int64_t* r) {
    bool any = false;
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* c = proc_at(i);
        if (!child_matches(p, c, pid)) continue;
        any = true;
        if (c->state == PROCESS_STATE_ZOMBIE) {
            if (rusage) {
                uint8_t z[144];
                memset(z, 0, sizeof(z));
                uaccess_write(p->address_space, rusage, z, sizeof(z));
            }
            *r = reap(p, c, ustatus);
            return LX_CONTINUE;
        }
    }
    if (!any) {
        *r = err(LX_ECHILD);
        return LX_CONTINUE;
    }
    if (options & WNOHANG) {
        *r = 0;
        return LX_CONTINUE;
    }
    p->context.rax = 0;
    machine_block(p, WAIT_CHILD, (uint64_t)pid, 0);
    p->wait_aux = ustatus;
    return LX_SWITCHED;
}

/* ---- kill ---- */

/* The thread a process-directed signal goes to: the leader, unless its own
 * thread has exited. */
static process_t* group_target(uint32_t tgid) {
    process_t* leader = proc_by_pid(tgid);
    if (!leader || leader->tgid != tgid) return 0;
    if (!(leader->state == PROCESS_STATE_BLOCKED && leader->wait_reason == WAIT_GROUP)) return leader;
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* t = proc_at(i);
        if (t->tgid == tgid && t != leader) return t;
    }
    return leader;
}

static lx_after_t do_kill(process_t* p, int64_t pid, uint64_t sig, int32_t code, int64_t* r) {
    if (sig > 64) { *r = err(LX_EINVAL); return LX_CONTINUE; }
    process_t* t;
    if (code == SI_TKILL) t = pid > 0 ? proc_by_pid((uint32_t)pid) : 0;
    else t = pid > 0 ? group_target((uint32_t)pid) : pid == 0 ? p : 0;
    if (!t) { *r = err(LX_ESRCH); return LX_CONTINUE; }
    *r = 0;
    if (sig == 0) return LX_CONTINUE;
    /* Pending until t next runs (#250); SIGKILL ends t now. */
    ksig_send(t, (uint32_t)sig, code, (uint64_t)p->pid);
    return t->tgid == p->tgid && sig == SIG_SIGKILL ? LX_SWITCHED : LX_CONTINUE;
}

/* ---- futex (#251) ---- */

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE         128
#define FUTEX_CLOCK_REALTIME  256
#define BITSET_ANY            0xFFFFFFFFu

/* Waiters are processes blocked in WAIT_FUTEX on (address space, address),
 * woken in table order: deterministic, like everything the scheduler does. */
static bool futex_waiter(const process_t* t, const process_t* p, uint64_t uaddr) {
    return t->state == PROCESS_STATE_BLOCKED && t->wait_reason == WAIT_FUTEX &&
           t->address_space == p->address_space && t->wait_obj == uaddr;
}

static int64_t futex_wake(process_t* p, uint64_t uaddr, uint32_t n, uint32_t bitset) {
    int64_t woken = 0;
    for (uint32_t i = 0; i < proc_count() && (uint32_t)woken < n; i++) {
        process_t* t = proc_at(i);
        if (!futex_waiter(t, p, uaddr) || !((uint32_t)t->wait_aux & bitset)) continue;
        machine_wake(t, 0);
        woken++;
    }
    return woken;
}

static int64_t read_timeout(process_t* p, uint64_t uts, uint64_t* ns) {
    int64_t ts[2];
    if (!uaccess_read(p->address_space, uts, ts, sizeof(ts))) return err(LX_EFAULT);
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000) return err(LX_EINVAL);
    *ns = (uint64_t)ts[0] * 1000000000ULL + (uint64_t)ts[1];
    return 0;
}

static lx_after_t sys_futex(process_t* p, uint64_t uaddr, uint64_t op, uint64_t val, uint64_t uts,
                            uint64_t uaddr2, uint64_t val3, int64_t* r) {
    uint32_t cmd = (uint32_t)op & ~(uint32_t)(FUTEX_PRIVATE | FUTEX_CLOCK_REALTIME);
    if (uaddr & 3) { *r = err(LX_EINVAL); return LX_CONTINUE; }
    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        uint32_t bitset = cmd == FUTEX_WAIT ? BITSET_ANY : (uint32_t)val3;
        if (!bitset) { *r = err(LX_EINVAL); return LX_CONTINUE; }
        uint32_t cur;
        if (!uaccess_read(p->address_space, uaddr, &cur, 4)) { *r = err(LX_EFAULT); return LX_CONTINUE; }
        if (cur != (uint32_t)val) { *r = err(LX_EAGAIN); return LX_CONTINUE; }
        uint64_t deadline = 0;
        if (uts) {
            uint64_t ns;
            if ((*r = read_timeout(p, uts, &ns)) < 0) return LX_CONTINUE;
            uint64_t now = clock_monotonic_ns();
            if (cmd == FUTEX_WAIT) {
                deadline = now + ns;                         /* relative */
            } else if (op & FUTEX_CLOCK_REALTIME) {
                uint64_t wall = clock_realtime_ns();
                deadline = ns > wall ? now + (ns - wall) : now;
            } else {
                deadline = ns;                               /* absolute, monotonic */
            }
            if (!deadline) deadline = 1;
        }
        p->context.rax = 0;
        machine_block(p, WAIT_FUTEX, uaddr, deadline);
        p->wait_aux = bitset;
        return LX_SWITCHED;
    }
    case FUTEX_WAKE:
        *r = futex_wake(p, uaddr, (uint32_t)val, BITSET_ANY);
        return LX_CONTINUE;
    case FUTEX_WAKE_BITSET:
        if (!(uint32_t)val3) { *r = err(LX_EINVAL); return LX_CONTINUE; }
        *r = futex_wake(p, uaddr, (uint32_t)val, (uint32_t)val3);
        return LX_CONTINUE;
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        if (cmd == FUTEX_CMP_REQUEUE) {
            uint32_t cur;
            if (!uaccess_read(p->address_space, uaddr, &cur, 4)) { *r = err(LX_EFAULT); return LX_CONTINUE; }
            if (cur != (uint32_t)val3) { *r = err(LX_EAGAIN); return LX_CONTINUE; }
        }
        int64_t n = futex_wake(p, uaddr, (uint32_t)val, BITSET_ANY);
        uint32_t limit = (uint32_t)uts;                    /* val2 rides in the timeout slot */
        for (uint32_t i = 0, moved = 0; i < proc_count() && moved < limit; i++) {
            process_t* t = proc_at(i);
            if (!futex_waiter(t, p, uaddr)) continue;
            t->wait_obj = uaddr2;
            moved++;
            n++;
        }
        *r = n;
        return LX_CONTINUE;
    }
    default:
        *r = err(LX_ENOSYS);
        return LX_CONTINUE;
    }
}

/* Thread exit (#251): CLONE_CHILD_CLEARTID's word is cleared and its waiter
 * woken (pthread_join), then the thread goes. */
static void thread_exit(process_t* p, int code) {
    uint64_t ctid = proc_arch(p)->clear_child_tid;
    if (ctid) {
        uint32_t zero = 0;
        if (uaccess_write(p->address_space, ctid, &zero, 4)) futex_wake(p, ctid, 1, BITSET_ANY);
    }
    machine_thread_exit(p, code);
}

/* ---- Dispatch ---- */

bool pctl_syscall(process_t* p, uint64_t nr, lx_after_t* after) {
    process_context_t* c = &p->context;
    uint64_t a0 = c->rdi, a1 = c->rsi, a2 = c->rdx, a3 = c->r10, a4 = c->r8;
    int64_t r = 0;
    lx_after_t a = LX_CONTINUE;
    switch (nr) {
    case LX_fork:
    case LX_vfork:
        r = sys_clone(p, SIGCHLD, 0, 0, 0, 0);
        break;
    case LX_clone:
        r = sys_clone(p, a0, a1, a2, a3, a4);
        break;
    case LX_execve:
        r = sys_execve(p, a0, a1, a2);
        if (r == 0) {
            *after = LX_SWITCHED;               /* the new program's registers stand */
            return true;
        }
        break;
    case LX_wait4:
        a = sys_wait4(p, (int64_t)a0, a1, a2, a3, &r);
        break;
    case LX_kill:
        a = do_kill(p, (int64_t)a0, a1, SI_USER, &r);
        break;
    case 200:                                   /* tkill */
        a = do_kill(p, (int64_t)a0, a1, SI_TKILL, &r);
        break;
    case 234:                                   /* tgkill */
        a = do_kill(p, (int64_t)a1, a2, SI_TKILL, &r);
        break;
    case LX_futex:
        a = sys_futex(p, a0, a1, a2, a3, a4, c->r9, &r);
        break;
    case LX_exit:
        thread_exit(p, (int)a0);
        *after = LX_SWITCHED;
        return true;
    case LX_exit_group:
        machine_terminate(p, (int)a0);
        *after = LX_SWITCHED;
        return true;
    case LX_getpid:  r = p->tgid; break;
    case LX_gettid:  r = p->pid; break;
    case LX_getppid: r = p->ppid; break;
    case LX_sched_getaffinity: {
        /* One CPU. */
        uint64_t mask = 1;
        if (a1 < 8) { r = err(LX_EINVAL); break; }
        r = uaccess_write(p->address_space, a2, &mask, 8) ? 8 : err(LX_EFAULT);
        break;
    }
    case 109: r = 0; break;                     /* setpgid */
    case 121:                                   /* getpgid */
    case 124:                                   /* getsid */
        r = a0 ? (proc_by_pid((uint32_t)a0) ? (int64_t)a0 : err(LX_ESRCH)) : p->tgid;
        break;
    case 111: r = p->tgid; break;               /* getpgrp */
    case 112: r = p->tgid; break;               /* setsid */
    default:
        return false;
    }
    if (a == LX_SWITCHED) {
        *after = LX_SWITCHED;
        return true;
    }
    c->rax = (uint64_t)r;
    *after = LX_CONTINUE;
    return true;
}
