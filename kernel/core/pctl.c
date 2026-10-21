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

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

static inline int64_t err(int e) { return -(int64_t)e; }

#define WNOHANG      1
#define SIGCHLD      17
#define CLONE_VM             0x00000100
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
    c->ppid = p->pid;
    c->entry_point = p->entry_point;
    c->stack_start = p->stack_start;
    c->stack_end = p->stack_end;
    memcpy(c->exe_path, p->exe_path, sizeof(c->exe_path));
    c->exe_crc = p->exe_crc;
    files_fork(p, c);
    c->state = PROCESS_STATE_READY;
    proc_append(c);
    return c;
}

static int64_t sys_clone(process_t* p, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t ctid,
                         uint64_t tls) {
    if (flags & CLONE_VM) return err(LX_ENOSYS);   /* threads: #251 */
    int64_t e = 0;
    process_t* c = do_fork(p, &e);
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
    rc = proc_exec(p, img, argc, argv, envc, envp);
    if (rc < 0) return rc;
    files_exec(p);                              /* close-on-exec descriptors */
    return 0;
}

/* ---- exit and wait ---- */

static bool child_matches(const process_t* parent, const process_t* c, int64_t pid) {
    if (c->ppid != parent->pid) return false;
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
    process_t* parent = proc_by_pid((uint32_t)c->ppid);
    if (!parent || parent->state != PROCESS_STATE_BLOCKED || parent->wait_reason != WAIT_CHILD) return;
    if (!child_matches(parent, c, (int64_t)parent->wait_obj)) return;
    int64_t r = reap(parent, c, parent->wait_aux);
    machine_wake(parent, r);
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

static bool default_ignored(uint64_t sig) {
    return sig == SIGCHLD || sig == 23 /* SIGURG */ || sig == 28 /* SIGWINCH */ ||
           sig == 18 /* SIGCONT */;
}

static lx_after_t do_kill(process_t* p, int64_t pid, uint64_t sig, int64_t* r) {
    if (sig > 64) { *r = err(LX_EINVAL); return LX_CONTINUE; }
    process_t* t = pid > 0 ? proc_by_pid((uint32_t)pid) : pid == 0 ? p : 0;
    if (!t) { *r = err(LX_ESRCH); return LX_CONTINUE; }
    *r = 0;
    if (sig == 0 || t->state == PROCESS_STATE_ZOMBIE || default_ignored(sig)) return LX_CONTINUE;
    /* No handlers yet (#250): every other signal ends the process. */
    machine_kill(t, (uint32_t)sig);
    return t == p ? LX_SWITCHED : LX_CONTINUE;
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
        a = do_kill(p, (int64_t)a0, a1, &r);
        break;
    case 200:                                   /* tkill */
        a = do_kill(p, (int64_t)a0, a1, &r);
        break;
    case 234:                                   /* tgkill */
        a = do_kill(p, (int64_t)a1, a2, &r);
        break;
    case 109: r = 0; break;                     /* setpgid */
    case 121:                                   /* getpgid */
    case 124:                                   /* getsid */
        r = a0 ? (proc_by_pid((uint32_t)a0) ? (int64_t)a0 : err(LX_ESRCH)) : p->pid;
        break;
    case 111: r = p->pid; break;                /* getpgrp */
    case 112: r = p->pid; break;                /* setsid */
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
