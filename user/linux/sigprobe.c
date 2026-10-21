/* Signals (#250).
 *
 *   sigprobe alarm   a 10 ms ITIMER_REAL; the SIGALRM handler (doing its own
 *                    floating point) interrupts a loop 20 times; the loop's
 *                    floating-point state must survive every interruption
 *   sigprobe segv    a SIGSEGV handler sees si_addr and recovers with
 *                    siglongjmp
 *   sigprobe mask    blocking, pending, sigsuspend woken by a child's kill,
 *                    SA_RESETHAND, and a handler on an alternate stack
 *   sigprobe null    a null-pointer store with no handler
 *   sigprobe abort   abort()
 */

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int g_ticks;
static volatile uint64_t g_where[32];      /* loop iteration at each tick */
static volatile uint64_t g_iter;
static volatile double g_handler_acc;

static void on_alarm(int sig) {
    (void)sig;
    double x = 1.0;
    for (int i = 1; i < 50; i++) x = x * 1.0001 + 1.0 / i;   /* clobbers the FPU registers */
    g_handler_acc += x;
    if (g_ticks < 32) g_where[g_ticks] = g_iter;
    g_ticks++;
}

static sigjmp_buf g_jb;
static volatile void* g_fault_addr;
static volatile int g_fault_code;
static void on_segv(int sig, siginfo_t* si, void* uc) {
    (void)sig; (void)uc;
    g_fault_addr = si->si_addr;
    g_fault_code = si->si_code;
    siglongjmp(g_jb, 1);
}

static volatile int g_usr1, g_usr2;
static volatile uintptr_t g_handler_sp;
static void on_usr1(int sig) { (void)sig; g_usr1++; }
static void on_usr2(int sig) {
    (void)sig;
    int local;
    g_handler_sp = (uintptr_t)&local;
    g_usr2++;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "";
    if (strcmp(mode, "alarm") == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_alarm;
        sigaction(SIGALRM, &sa, 0);
        struct itimerval it = { { 0, 10000 }, { 0, 10000 } };
        setitimer(ITIMER_REAL, &it, 0);
        double acc = 0.0;
        while (g_ticks < 20) {
            acc = acc * 0.5 + 1.25;            /* converges to 2.5 */
            g_iter++;
            if ((g_iter & 1023) == 0) getppid();   /* a kernel entry: a delivery point */
        }
        struct itimerval off = { { 0, 0 }, { 0, 0 } };
        setitimer(ITIMER_REAL, &off, 0);
        int spread = 0;
        for (int i = 1; i < 20; i++) spread += g_where[i] > g_where[i - 1];
        printf("sigprobe: %d SIGALRMs interrupted the loop at %d distinct points; loop FPU %s, "
               "handler FPU %s\n", g_ticks, spread + 1, acc == 2.5 ? "intact" : "CORRUPTED",
               g_handler_acc > 0 ? "worked" : "BROKEN");
        return 0;
    }
    if (strcmp(mode, "segv") == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_segv;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, 0);
        if (sigsetjmp(g_jb, 1) == 0) {
            *(volatile int*)0x10 = 1;
            printf("sigprobe: no fault?\n");
            return 1;
        }
        printf("sigprobe: caught SIGSEGV at %p (code %d) and recovered\n", (void*)g_fault_addr, g_fault_code);
        return 0;
    }
    if (strcmp(mode, "mask") == 0) {
        signal(SIGUSR1, on_usr1);
        sigset_t s, old;
        sigemptyset(&s);
        sigaddset(&s, SIGUSR1);
        sigprocmask(SIG_BLOCK, &s, &old);
        kill(getpid(), SIGUSR1);
        sigset_t pend;
        sigpending(&pend);
        int was_pending = sigismember(&pend, SIGUSR1) && g_usr1 == 0;
        sigprocmask(SIG_SETMASK, &old, 0);
        int ran_on_unblock = g_usr1 == 1;

        /* sigsuspend, woken by a child's SIGUSR2, on the alternate stack. */
        static char alt[65536];
        stack_t ss = { .ss_sp = alt, .ss_size = sizeof(alt), .ss_flags = 0 };
        sigaltstack(&ss, 0);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr2;
        sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
        sigaction(SIGUSR2, &sa, 0);
        sigset_t block2, waitmask;
        sigemptyset(&block2);
        sigaddset(&block2, SIGUSR2);
        sigprocmask(SIG_BLOCK, &block2, 0);
        pid_t parent = getpid();
        pid_t c = fork();
        if (c == 0) {
            struct timespec d = { 0, 30000000 };
            nanosleep(&d, 0);
            kill(parent, SIGUSR2);
            _exit(0);
        }
        sigemptyset(&waitmask);
        int rc = sigsuspend(&waitmask);
        int on_alt = g_handler_sp >= (uintptr_t)alt && g_handler_sp < (uintptr_t)alt + sizeof(alt);
        waitpid(c, 0, 0);
        struct sigaction now;
        sigaction(SIGUSR2, 0, &now);
        printf("sigprobe: blocked SIGUSR1 %s, delivered on unblock %s; sigsuspend returned %d after "
               "SIGUSR2, handler on the alternate stack %s, reset to default %s\n",
               was_pending ? "pending" : "NOT pending", ran_on_unblock ? "yes" : "NO", rc,
               on_alt ? "yes" : "NO", now.sa_handler == SIG_DFL ? "yes" : "NO");
        return 0;
    }
    if (strcmp(mode, "null") == 0) {
        printf("sigprobe: storing through a null pointer\n");
        volatile int* p = 0;
        *p = 42;                               /* the faulting line */
        printf("sigprobe: still running?\n");
        return 1;
    }
    if (strcmp(mode, "abort") == 0) {
        printf("sigprobe: calling abort()\n");
        abort();
    }
    printf("usage: sigprobe alarm | segv | mask | null | abort\n");
    return 2;
}
