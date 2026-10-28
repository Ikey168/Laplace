/* The first unmodified Linux program (#238): plain C against musl, built with
 * `musl-gcc -static`. Exercises the startup path (auxv, TLS via arch_prctl,
 * set_tid_address), stdio (writev), the clock, and getrandom. */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/random.h>
#include <sys/utsname.h>
#include <unistd.h>

static __thread int tls_counter = 41;
volatile int g_greeted;   /* set before the greeting: a store the gates find backward (#256) */
/* The random bytes, kept: a replay that reads other ones ends in another
 * state (the stack copy is overwritten before the epoch ends). */
volatile unsigned char g_random[8];

int main(int argc, char** argv) {
    struct utsname u;
    uname(&u);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned char r[8];
    ssize_t n = getrandom(r, sizeof(r), 0);
    for (int i = 0; i < 8; i++) g_random[i] = r[i];
    tls_counter++;
    g_greeted = 1;
    printf("hello from %s on %s %s (argc %d, pid %d, tls %d, getrandom %zd, clock %s)\n",
           argv[0], u.sysname, u.machine, argc, (int)getpid(), tls_counter, n,
           ts.tv_sec >= 0 ? "ok" : "bad");
    return tls_counter == 42 ? 0 : 1;
}
