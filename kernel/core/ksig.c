/* Laplace core: POSIX signals for Linux programs (#250). See include/core/ksig.h. */

#include "core/ksig.h"
#include "core/kstate.h"
#include "core/machine.h"
#include "core/proc.h"
#include "core/mm.h"
#include "core/cpu.h"
#include "core/fpu.h"
#include "core/clock.h"
#include "core/console.h"
#include "divergence.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

static inline int64_t err(int e) { return -(int64_t)e; }

#define NSIG          64
#define NTIMERS       8              /* [0]: ITIMER_REAL; [1..]: timer_create */
#define SIG_DFL       0
#define SIG_IGN       1

#define SA_SIGINFO    0x00000004
#define SA_RESTORER   0x04000000
#define SA_ONSTACK    0x08000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000u

#define SS_ONSTACK    1
#define SS_DISABLE    2
#define MINSIGSTKSZ   2048

#define BIT(s)        (1ULL << ((s) - 1))
#define UNBLOCKABLE   (BIT(SIG_SIGKILL) | BIT(SIG_SIGSTOP))

typedef struct {
    uint64_t handler, flags, restorer, mask;
} ksigaction_t;

typedef struct {
    uint32_t armed;
    uint32_t signo;
    uint64_t deadline;        /* monotonic ns */
    uint64_t interval;
} ktimer_t;

typedef struct {
    ksigaction_t act[NSIG + 1];
    uint64_t     blocked;
    uint64_t     pending;
    int32_t      code[NSIG + 1];
    uint64_t     val[NSIG + 1];   /* si_addr, sender pid, or timer index */
    uint32_t     sync_sig;        /* a fault's signal, delivered first */
    uint32_t     in_suspend;      /* rt_sigsuspend: saved_mask is the real one */
    uint64_t     saved_mask;
    uint64_t     ss_sp, ss_size;
    uint32_t     ss_flags;
    uint32_t     ntimers_used;
    ktimer_t     timer[NTIMERS];
} sigstate_t;

static sigstate_t* st(const process_t* p) { return (sigstate_t*)p->sig; }

static const char* g_names[NSIG + 1] = {
    "0", "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP", "SIGABRT", "SIGBUS", "SIGFPE",
    "SIGKILL", "SIGUSR1", "SIGSEGV", "SIGUSR2", "SIGPIPE", "SIGALRM", "SIGTERM", "SIGSTKFLT",
    "SIGCHLD", "SIGCONT", "SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU", "SIGURG", "SIGXCPU",
    "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH", "SIGIO", "SIGPWR", "SIGSYS",
};

const char* ksig_name(uint32_t sig) {
    if (sig <= 31 && g_names[sig]) return g_names[sig];
    return sig <= NSIG ? "SIGRT" : "?";
}

/* ---- Default actions ---- */

enum { DFL_TERM, DFL_CORE, DFL_IGNORE };

static int default_action(uint32_t sig) {
    switch (sig) {
    case SIG_SIGCHLD: case SIG_SIGCONT: case 23 /* URG */: case 28 /* WINCH */:
    case SIG_SIGSTOP: case 20: case 21: case 22:     /* no job control: stops do nothing */
        return DFL_IGNORE;
    case SIG_SIGQUIT: case SIG_SIGILL: case SIG_SIGTRAP: case SIG_SIGABRT: case SIG_SIGBUS:
    case SIG_SIGFPE: case SIG_SIGSEGV: case 24: case 25: case 31:
        return DFL_CORE;
    default:
        return DFL_TERM;
    }
}

static bool ignored(const sigstate_t* s, uint32_t sig) {
    uint64_t h = s->act[sig].handler;
    return h == SIG_IGN || (h == SIG_DFL && default_action(sig) == DFL_IGNORE);
}

/* ---- Lifecycle ---- */

void ksig_proc_new(process_t* p) {
    if (p->sig) return;
    p->sig = kzalloc(sizeof(sigstate_t));
    if (!p->sig) panic("signals: out of memory");
    st(p)->ss_flags = SS_DISABLE;
}

void ksig_proc_gone(process_t* p) {
    if (p->sig) kfree(p->sig);
    p->sig = 0;
}

void ksig_fork(process_t* parent, process_t* child) {
    ksig_proc_new(child);
    if (!parent->sig) return;
    sigstate_t* c = st(child);
    memcpy(c->act, st(parent)->act, sizeof(c->act));
    c->blocked = st(parent)->blocked;
    c->ss_sp = st(parent)->ss_sp;
    c->ss_size = st(parent)->ss_size;
    c->ss_flags = st(parent)->ss_flags;
    /* Pending signals and timers are the parent's own. */
}

void ksig_thread(process_t* parent, process_t* child) {
    ksig_proc_new(child);
    if (!parent->sig) return;
    sigstate_t* c = st(child);
    memcpy(c->act, st(parent)->act, sizeof(c->act));
    c->blocked = st(parent)->blocked;
    /* No alternate stack, pending signals, or timers of its own yet. */
}

/* Handlers belong to the thread group (#251): every thread's copy changes. */
static void set_action(process_t* p, uint32_t sig, const ksigaction_t* a) {
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* t = proc_at(i);
        if (t->tgid != p->tgid || !t->sig) continue;
        st(t)->act[sig] = *a;
        if (ignored(st(t), sig)) st(t)->pending &= ~BIT(sig);
    }
}

void ksig_exec(process_t* p) {
    sigstate_t* s = st(p);
    if (!s) return;
    for (uint32_t i = 1; i <= NSIG; i++) {
        if (s->act[i].handler != SIG_IGN) memset(&s->act[i], 0, sizeof(s->act[i]));
    }
    s->ss_flags = SS_DISABLE;
    s->ss_sp = s->ss_size = 0;
    for (uint32_t i = 1; i < NTIMERS; i++) memset(&s->timer[i], 0, sizeof(s->timer[i]));
}

/* ---- Sending ---- */

void ksig_send(process_t* t, uint32_t sig, int32_t code, uint64_t val) {
    if (t && t->state == PROCESS_STATE_BLOCKED && t->wait_reason == WAIT_GROUP) {
        /* A leader whose thread exited: another thread of its group takes
         * the signal (#251). */
        for (uint32_t i = 0; i < proc_count(); i++) {
            process_t* o = proc_at(i);
            if (o->tgid == t->tgid && o != t && o->state != PROCESS_STATE_ZOMBIE) { t = o; break; }
        }
    }
    if (!t || sig == 0 || sig > NSIG || t->state == PROCESS_STATE_ZOMBIE) return;
    if (sig == SIG_SIGKILL) {
        machine_kill(t, SIG_SIGKILL);
        return;
    }
    sigstate_t* s = st(t);
    if (!s || ignored(s, sig)) return;
    s->pending |= BIT(sig);
    s->code[sig] = code;
    s->val[sig] = val;
    if (t->state == PROCESS_STATE_BLOCKED && !(s->blocked & BIT(sig))) {
        machine_wake(t, err(LX_EINTR));
    }
}

bool ksig_fault_handled(const process_t* p, uint32_t sig) {
    const sigstate_t* s = st(p);
    if (!s || sig == 0 || sig > NSIG) return false;
    uint64_t h = s->act[sig].handler;
    return h != SIG_DFL && h != SIG_IGN && !(s->blocked & BIT(sig));
}

bool ksig_fault(process_t* p, uint32_t sig, int32_t code, uint64_t addr) {
    if (!ksig_fault_handled(p, sig)) return false;
    sigstate_t* s = st(p);
    s->pending |= BIT(sig);
    s->code[sig] = code;
    s->val[sig] = addr;
    s->sync_sig = sig;
    return true;
}

/* ---- Delivery ---- */

#define FRAME_UC     8
#define FRAME_INFO   (8 + 304)
#define FRAME_SIZE   (8 + 304 + 128)
#define UC_SIGCTX    40
#define UC_MASK      296

static void put64(uint8_t* b, uint32_t off, uint64_t v) { memcpy(b + off, &v, 8); }
static void put32(uint8_t* b, uint32_t off, uint32_t v) { memcpy(b + off, &v, 4); }
static void put16(uint8_t* b, uint32_t off, uint16_t v) { memcpy(b + off, &v, 2); }
static uint64_t get64(const uint8_t* b, uint32_t off) { uint64_t v; memcpy(&v, b + off, 8); return v; }

static bool on_altstack(const sigstate_t* s, uint64_t sp) {
    return !(s->ss_flags & SS_DISABLE) && sp > s->ss_sp && sp <= s->ss_sp + s->ss_size;
}

static bool build_frame(process_t* p, uint32_t sig) {
    sigstate_t* s = st(p);
    ksigaction_t* a = &s->act[sig];
    process_context_t* c = &p->context;
    uint64_t sp = c->rsp;
    if ((a->flags & SA_ONSTACK) && !(s->ss_flags & SS_DISABLE) && !on_altstack(s, sp)) {
        sp = s->ss_sp + s->ss_size;
    } else {
        sp -= 128;                                      /* the red zone */
    }
    /* The extended FPU state, with the XSAVE markers when XSAVE is in use. */
    machine_fpu_flush();
    uint8_t* area = proc_arch(p)->fpu;
    uint32_t fsize = fpu_area_size();
    bool xs = fpu_has_xsave();
    static uint8_t fp[FPU_AREA_MAX + 4];
    memcpy(fp, area, fsize);
    if (xs) {
        put32(fp, 464, 0x46505853u);                    /* FP_XSTATE_MAGIC1 */
        put32(fp, 468, fsize + 4);                      /* extended_size */
        put64(fp, 472, fpu_xcr0());                     /* xfeatures */
        put32(fp, 480, fsize);                          /* xstate_size */
        put32(fp, fsize, 0x46505845u);                  /* FP_XSTATE_MAGIC2 */
    }
    uint32_t fbytes = fsize + (xs ? 4 : 0);
    sp = (sp - fbytes) & ~63ULL;
    uint64_t fpaddr = sp;
    sp = ((sp - FRAME_SIZE) & ~15ULL) - 8;
    uint64_t frame = sp;

    uint8_t f[FRAME_SIZE];
    memset(f, 0, sizeof(f));
    put64(f, 0, a->restorer);                           /* pretcode */
    uint8_t* uc = f + FRAME_UC;
    put64(uc, 0, xs ? 1 : 0);                           /* uc_flags: UC_FP_XSTATE */
    put64(uc, 16, s->ss_sp);                            /* uc_stack */
    put32(uc, 24, (s->ss_flags & SS_DISABLE) ? SS_DISABLE : on_altstack(s, c->rsp) ? SS_ONSTACK : 0);
    put64(uc, 32, s->ss_size);
    uint8_t* sc = uc + UC_SIGCTX;
    const uint64_t regs[18] = { c->r8, c->r9, c->r10, c->r11, c->r12, c->r13, c->r14, c->r15,
                                c->rdi, c->rsi, c->rbp, c->rbx, c->rdx, c->rax, c->rcx, c->rsp,
                                c->rip, c->rflags };
    for (int i = 0; i < 18; i++) put64(sc, (uint32_t)i * 8, regs[i]);
    put16(sc, 144, SEL_UCODE);
    put16(sc, 150, SEL_UDATA);
    const pending_entry_t* pe = machine_pending();
    bool fault = s->sync_sig == sig;
    put64(sc, 152, fault ? pe->error : 0);
    put64(sc, 160, fault ? pe->vector : 0);
    put64(sc, 168, s->in_suspend ? s->saved_mask : s->blocked);   /* oldmask */
    put64(sc, 176, fault ? pe->cr2 : 0);
    put64(sc, 184, fpaddr);
    put64(uc, UC_MASK, s->in_suspend ? s->saved_mask : s->blocked);
    uint8_t* si = f + FRAME_INFO;
    put32(si, 0, sig);
    put32(si, 8, (uint32_t)s->code[sig]);
    if (fault || sig == SIG_SIGBUS) {
        put64(si, 16, s->val[sig]);                     /* si_addr */
    } else if (s->code[sig] == SI_TIMER) {
        put32(si, 16, (uint32_t)s->val[sig]);           /* si_tid: the timer */
    } else {
        put32(si, 16, (uint32_t)s->val[sig]);           /* si_pid */
    }
    if (!uaccess_write(p->address_space, fpaddr, fp, fbytes) ||
        !uaccess_write(p->address_space, frame, f, sizeof(f))) {
        return false;
    }

    /* The handler runs on the frame with a clean FPU. */
    machine_fpu_forget(p);
    fpu_area_init(area);
    c->rip = a->handler;
    c->rsp = frame;
    c->rdi = sig;
    c->rsi = frame + FRAME_INFO;
    c->rdx = frame + FRAME_UC;
    c->rax = 0;
    c->rflags &= ~0x400ULL;                             /* DF */
    uint64_t mask = s->in_suspend ? s->saved_mask : s->blocked;
    mask |= a->mask;
    if (!(a->flags & SA_NODEFER)) mask |= BIT(sig);
    s->blocked = mask & ~UNBLOCKABLE;
    s->in_suspend = 0;
    if (a->flags & SA_RESETHAND) {
        ksigaction_t dfl;
        memset(&dfl, 0, sizeof(dfl));
        set_action(p, sig, &dfl);
    }
    return true;
}

ksig_result_t ksig_deliver(process_t* p, uint32_t* out) {
    sigstate_t* s = st(p);
    if (!s) return KSIG_NONE;
    uint64_t ready = s->pending & ~s->blocked;
    if (!ready) return KSIG_NONE;
    uint32_t sig = s->sync_sig && (ready & BIT(s->sync_sig)) ? s->sync_sig : (uint32_t)__builtin_ctzll(ready) + 1;
    *out = sig;
    s->pending &= ~BIT(sig);
    uint64_t h = s->act[sig].handler;
    if (h == SIG_IGN) {
        s->sync_sig = 0;
        return KSIG_NONE;
    }
    if (h == SIG_DFL) {
        s->sync_sig = 0;
        switch (default_action(sig)) {
        case DFL_IGNORE: return KSIG_NONE;
        case DFL_CORE: return KSIG_STOP;
        default:
            machine_kill(p, sig);
            return KSIG_KILLED;
        }
    }
    bool ok = build_frame(p, sig);
    s->sync_sig = 0;
    if (!ok) {
        /* No room for the frame: the process dies of a fault. */
        machine_kill(p, SIG_SIGSEGV);
        return KSIG_KILLED;
    }
    return KSIG_DELIVERED;
}

static void sys_sigreturn(process_t* p) {
    sigstate_t* s = st(p);
    process_context_t* c = &p->context;
    uint64_t uc = c->rsp;                               /* the restorer's ret popped pretcode */
    uint8_t u[304];
    if (!uaccess_read(p->address_space, uc, u, sizeof(u))) {
        machine_kill(p, SIG_SIGSEGV);
        return;
    }
    const uint8_t* sc = u + UC_SIGCTX;
    c->r8 = get64(sc, 0);   c->r9 = get64(sc, 8);    c->r10 = get64(sc, 16); c->r11 = get64(sc, 24);
    c->r12 = get64(sc, 32); c->r13 = get64(sc, 40);  c->r14 = get64(sc, 48); c->r15 = get64(sc, 56);
    c->rdi = get64(sc, 64); c->rsi = get64(sc, 72);  c->rbp = get64(sc, 80); c->rbx = get64(sc, 88);
    c->rdx = get64(sc, 96); c->rax = get64(sc, 104); c->rcx = get64(sc, 112);
    c->rsp = get64(sc, 120);
    c->rip = get64(sc, 128);
    c->rflags = (get64(sc, 136) & 0x40DD5ULL) | 0x202;
    c->cs = SEL_UCODE;
    c->ss = SEL_UDATA;
    s->blocked = get64(u, UC_MASK) & ~UNBLOCKABLE;
    uint64_t fpaddr = get64(sc, 184);
    if (fpaddr) {
        uint8_t* area = proc_arch(p)->fpu;
        machine_fpu_forget(p);
        if (uaccess_read(p->address_space, fpaddr, area, fpu_area_size())) {
            fpu_sanitize(area);
        } else {
            fpu_area_init(area);
        }
    }
}

/* ---- Timers ---- */

bool ksig_next_deadline(const process_t* p, uint64_t* deadline) {
    const sigstate_t* s = st(p);
    if (!s || p->state == PROCESS_STATE_ZOMBIE) return false;
    bool any = false;
    for (uint32_t i = 0; i < NTIMERS; i++) {
        if (!s->timer[i].armed) continue;
        if (!any || s->timer[i].deadline < *deadline) *deadline = s->timer[i].deadline;
        any = true;
    }
    return any;
}

void ksig_fire(process_t* p, uint32_t index) {
    sigstate_t* s = st(p);
    if (!s || index >= NTIMERS || !s->timer[index].armed) return;
    ktimer_t* t = &s->timer[index];
    if (t->interval) t->deadline += t->interval;       /* no catching up: replays agree */
    else t->armed = 0;
    if (t->signo) ksig_send(p, t->signo, SI_TIMER, index);
}

void ksig_fire_due(process_t* p, uint64_t now, void (*fired)(uint32_t pid, uint32_t index)) {
    sigstate_t* s = st(p);
    if (!s || p->state == PROCESS_STATE_ZOMBIE) return;
    for (uint32_t i = 0; i < NTIMERS; i++) {
        if (!s->timer[i].armed || s->timer[i].deadline > now) continue;
        fired((uint32_t)p->pid, i);
        ksig_fire(p, i);
    }
}

/* ---- System calls ---- */

static int64_t sys_rt_sigaction(process_t* p, uint64_t sig, uint64_t act, uint64_t oact, uint64_t size) {
    sigstate_t* s = st(p);
    if (size != 8 || sig == 0 || sig > NSIG) return err(LX_EINVAL);
    if (oact && !uaccess_write(p->address_space, oact, &s->act[sig], sizeof(ksigaction_t))) return err(LX_EFAULT);
    if (!act) return 0;
    if (sig == SIG_SIGKILL || sig == SIG_SIGSTOP) return err(LX_EINVAL);
    ksigaction_t a;
    if (!uaccess_read(p->address_space, act, &a, sizeof(a))) return err(LX_EFAULT);
    a.mask &= ~UNBLOCKABLE;
    set_action(p, sig, &a);                           /* ignored: discarded now */
    return 0;
}

static int64_t sys_rt_sigprocmask(process_t* p, uint64_t how, uint64_t set, uint64_t oset, uint64_t size) {
    sigstate_t* s = st(p);
    if (size != 8) return err(LX_EINVAL);
    uint64_t old = s->blocked;
    if (set) {
        uint64_t m;
        if (!uaccess_read(p->address_space, set, &m, 8)) return err(LX_EFAULT);
        switch (how) {
        case 0: s->blocked |= m; break;
        case 1: s->blocked &= ~m; break;
        case 2: s->blocked = m; break;
        default: return err(LX_EINVAL);
        }
        s->blocked &= ~UNBLOCKABLE;
    }
    if (oset && !uaccess_write(p->address_space, oset, &old, 8)) return err(LX_EFAULT);
    return 0;
}

static int64_t sys_sigaltstack(process_t* p, uint64_t ss, uint64_t oss) {
    sigstate_t* s = st(p);
    if (oss) {
        uint8_t o[24];
        memset(o, 0, sizeof(o));
        put64(o, 0, s->ss_sp);
        put32(o, 8, (s->ss_flags & SS_DISABLE) ? SS_DISABLE : on_altstack(s, p->context.rsp) ? SS_ONSTACK : 0);
        put64(o, 16, s->ss_size);
        if (!uaccess_write(p->address_space, oss, o, sizeof(o))) return err(LX_EFAULT);
    }
    if (ss) {
        uint8_t n[24];
        if (!uaccess_read(p->address_space, ss, n, sizeof(n))) return err(LX_EFAULT);
        if (on_altstack(s, p->context.rsp)) return err(LX_EPERM);
        uint32_t flags;
        memcpy(&flags, n + 8, 4);
        if (flags & ~(uint32_t)SS_DISABLE) return err(LX_EINVAL);
        if (flags & SS_DISABLE) {
            s->ss_flags = SS_DISABLE;
            s->ss_sp = s->ss_size = 0;
        } else {
            uint64_t size = get64(n, 16);
            if (size < MINSIGSTKSZ) return err(12 /* ENOMEM */);
            s->ss_sp = get64(n, 0);
            s->ss_size = size;
            s->ss_flags = 0;
        }
    }
    return 0;
}

static lx_after_t sys_rt_sigsuspend(process_t* p, uint64_t mask, uint64_t size, int64_t* r) {
    sigstate_t* s = st(p);
    uint64_t m = s->blocked;
    if (mask) {
        if (size != 8) { *r = err(LX_EINVAL); return LX_CONTINUE; }
        if (!uaccess_read(p->address_space, mask, &m, 8)) { *r = err(LX_EFAULT); return LX_CONTINUE; }
    }
    s->saved_mask = s->blocked;
    s->in_suspend = 1;
    s->blocked = m & ~UNBLOCKABLE;
    p->context.rax = (uint64_t)err(LX_EINTR);
    if (s->pending & ~s->blocked) return LX_CONTINUE_KEEP;   /* deliverable already */
    machine_block(p, WAIT_SIGNAL, 0, 0);
    return LX_SWITCHED;
}

/* itimerval / itimerspec <-> ns. */
static int64_t read_pair(process_t* p, uint64_t uaddr, bool usec, uint64_t* interval, uint64_t* value) {
    int64_t t[4];
    if (!uaccess_read(p->address_space, uaddr, t, sizeof(t))) return err(LX_EFAULT);
    uint64_t scale = usec ? 1000 : 1;
    if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[3] < 0) return err(LX_EINVAL);
    *interval = (uint64_t)t[0] * 1000000000ULL + (uint64_t)t[1] * scale;
    *value = (uint64_t)t[2] * 1000000000ULL + (uint64_t)t[3] * scale;
    return 0;
}

static int64_t write_pair(process_t* p, uint64_t uaddr, bool usec, uint64_t interval, uint64_t value) {
    if (!uaddr) return 0;
    uint64_t scale = usec ? 1000 : 1;
    int64_t t[4] = { (int64_t)(interval / 1000000000ULL), (int64_t)((interval % 1000000000ULL) / scale),
                     (int64_t)(value / 1000000000ULL), (int64_t)((value % 1000000000ULL) / scale) };
    return uaccess_write(p->address_space, uaddr, t, sizeof(t)) ? 0 : err(LX_EFAULT);
}

static uint64_t remaining(const ktimer_t* t, uint64_t now) {
    return t->armed && t->deadline > now ? t->deadline - now : (t->armed ? 1 : 0);
}

static int64_t set_timer(process_t* p, uint32_t i, uint64_t interval, uint64_t value, bool abs_time,
                         uint64_t old_addr, bool usec) {
    ktimer_t* t = &st(p)->timer[i];
    uint64_t now = clock_monotonic_ns();
    int64_t rc = write_pair(p, old_addr, usec, t->interval, remaining(t, now));
    if (rc < 0) return rc;
    t->interval = interval;
    if (value == 0) {
        t->armed = 0;
    } else {
        t->armed = 1;
        t->deadline = abs_time ? value : now + value;
    }
    return 0;
}

static int64_t sys_timer_create(process_t* p, uint64_t clk, uint64_t sevp, uint64_t idp) {
    sigstate_t* s = st(p);
    if (clk > 7) return err(LX_EINVAL);
    uint32_t signo = SIG_SIGALRM;
    if (sevp) {
        uint8_t ev[16];
        if (!uaccess_read(p->address_space, sevp, ev, sizeof(ev))) return err(LX_EFAULT);
        uint32_t notify;
        memcpy(&signo, ev + 8, 4);
        memcpy(&notify, ev + 12, 4);
        if (notify == 1) signo = 0;                    /* SIGEV_NONE */
        else if (notify != 0) return err(LX_EINVAL);   /* threads, thread ids: not yet */
        if (signo > NSIG) return err(LX_EINVAL);
    }
    for (uint32_t i = 1; i < NTIMERS; i++) {
        if (s->ntimers_used & (1u << i)) continue;
        s->ntimers_used |= 1u << i;
        memset(&s->timer[i], 0, sizeof(s->timer[i]));
        s->timer[i].signo = signo;
        int32_t id = (int32_t)(i - 1);
        if (!uaccess_write(p->address_space, idp, &id, 4)) return err(LX_EFAULT);
        return 0;
    }
    return err(LX_EAGAIN);
}

static bool timer_index(process_t* p, uint64_t id, uint32_t* i) {
    if (id >= NTIMERS - 1) return false;
    *i = (uint32_t)id + 1;
    return (st(p)->ntimers_used & (1u << *i)) != 0;
}

bool ksig_syscall(process_t* p, uint64_t nr, lx_after_t* after) {
    process_context_t* c = &p->context;
    uint64_t a0 = c->rdi, a1 = c->rsi, a2 = c->rdx, a3 = c->r10;
    if (!st(p)) ksig_proc_new(p);
    sigstate_t* s = st(p);
    int64_t r = 0;
    uint32_t ti;
    switch (nr) {
    case 13: r = sys_rt_sigaction(p, a0, a1, a2, a3); break;          /* rt_sigaction */
    case 14: r = sys_rt_sigprocmask(p, a0, a1, a2, a3); break;        /* rt_sigprocmask */
    case 15:                                                         /* rt_sigreturn */
        sys_sigreturn(p);
        *after = LX_SWITCHED;                                        /* the frame's registers stand */
        return true;
    case 131: r = sys_sigaltstack(p, a0, a1); break;                  /* sigaltstack */
    case 127: {                                                      /* rt_sigpending */
        uint64_t pend = s->pending & s->blocked;
        r = a1 != 8 ? err(LX_EINVAL)
                    : uaccess_write(p->address_space, a0, &pend, 8) ? 0 : err(LX_EFAULT);
        break;
    }
    case 130: {                                                      /* rt_sigsuspend */
        lx_after_t a = sys_rt_sigsuspend(p, a0, a1, &r);
        if (a == LX_SWITCHED || a == LX_CONTINUE_KEEP) {
            *after = a == LX_SWITCHED ? LX_SWITCHED : LX_CONTINUE_KEEP;
            return true;
        }
        break;
    }
    case 34: {                                                       /* pause */
        lx_after_t a = sys_rt_sigsuspend(p, 0, 8, &r);
        *after = a == LX_SWITCHED ? LX_SWITCHED : LX_CONTINUE_KEEP;
        return true;
    }
    case 38:                                                         /* setitimer */
        if (a0 != 0) { r = a0 > 2 ? err(LX_EINVAL) : 0; break; }      /* only ITIMER_REAL runs */
        {
            uint64_t iv = 0, v = 0;
            if (a1 && (r = read_pair(p, a1, true, &iv, &v)) < 0) break;
            st(p)->timer[0].signo = SIG_SIGALRM;
            r = set_timer(p, 0, iv, v, false, a2, true);
        }
        break;
    case 36:                                                         /* getitimer */
        if (a0 != 0) { r = a0 > 2 ? err(LX_EINVAL) : write_pair(p, a1, true, 0, 0); break; }
        r = write_pair(p, a1, true, s->timer[0].interval, remaining(&s->timer[0], clock_monotonic_ns()));
        break;
    case 37: {                                                       /* alarm */
        uint64_t now = clock_monotonic_ns();
        uint64_t left = remaining(&s->timer[0], now);
        s->timer[0].signo = SIG_SIGALRM;
        s->timer[0].interval = 0;
        s->timer[0].armed = a0 != 0;
        s->timer[0].deadline = now + a0 * 1000000000ULL;
        r = (int64_t)((left + 999999999ULL) / 1000000000ULL);
        break;
    }
    case 222: r = sys_timer_create(p, a0, a1, a2); break;            /* timer_create */
    case 223: {                                                      /* timer_settime */
        if (!timer_index(p, a0, &ti)) { r = err(LX_EINVAL); break; }
        uint64_t iv, v;
        if ((r = read_pair(p, a2, false, &iv, &v)) < 0) break;
        uint64_t abs_v = v;
        if ((a1 & 1) && v) {
            /* TIMER_ABSTIME on the monotonic clock; wall times by distance. */
            abs_v = v;
        }
        r = set_timer(p, ti, iv, abs_v, (a1 & 1) != 0, a3, false);
        break;
    }
    case 224:                                                        /* timer_gettime */
        if (!timer_index(p, a0, &ti)) { r = err(LX_EINVAL); break; }
        r = write_pair(p, a1, false, s->timer[ti].interval, remaining(&s->timer[ti], clock_monotonic_ns()));
        break;
    case 225: r = timer_index(p, a0, &ti) ? 0 : err(LX_EINVAL); break;  /* timer_getoverrun */
    case 226:                                                        /* timer_delete */
        if (!timer_index(p, a0, &ti)) { r = err(LX_EINVAL); break; }
        s->ntimers_used &= ~(1u << ti);
        memset(&s->timer[ti], 0, sizeof(s->timer[ti]));
        break;
    default:
        return false;
    }
    c->rax = (uint64_t)r;
    *after = LX_CONTINUE_KEEP;
    return true;
}

/* ---- Keyframes ---- */

static void sig_save(ksf_writer_t* w, void* ctx) {
    (void)ctx;
    ksf_put_u32(w, proc_count());
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* p = proc_at(i);
        ksf_put_u32(w, (uint32_t)p->pid);
        ksf_put_u32(w, p->sig ? 1 : 0);
        if (p->sig) ksf_put(w, p->sig, sizeof(sigstate_t));
    }
}

static int sig_restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    if (version != 1) return -1;
    uint32_t n = ksf_get_u32(r);
    for (uint32_t k = 0; k < n && r->ok; k++) {
        process_t* p = proc_by_pid(ksf_get_u32(r));
        bool has = ksf_get_u32(r) != 0;
        if (!p) return -1;
        if (!has) continue;
        ksig_proc_new(p);
        if (!ksf_get(r, p->sig, sizeof(sigstate_t))) return -1;
    }
    return r->ok ? 0 : -1;
}

static uint32_t sig_sum(void* ctx) {
    (void)ctx;
    uint32_t crc = 0;
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* p = proc_at(i);
        uint32_t pid = (uint32_t)p->pid;
        crc = divergence_checksum(crc, &pid, sizeof(pid));
        if (p->sig) crc = divergence_checksum(crc, p->sig, sizeof(sigstate_t));
    }
    return crc;
}

void ksig_init(void) {
    static const ksf_subsys_t sub = { KS_ID_SIGNALS, "signals", 1, sig_save, sig_restore, sig_sum, 0 };
    if (kstate_register(&sub) != KSF_OK) panic("signals: cannot register the keyframe section");
}
