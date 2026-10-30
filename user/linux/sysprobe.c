/* System call probe (#238): an unimplemented call reports ENOSYS and the
 * program carries on; time moves forward and agrees across the clock calls;
 * getrandom fills its buffer; TLS works per process. */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static __thread unsigned long tls_value;
/* Kept in static memory, so the end state a replay is compared on depends on
 * every clock and random value the program read. */
static struct timespec g_a, g_b;
static struct timeval g_tv;
static time_t g_t;
static unsigned char g_buf[64];

int main(void) {
    long r = syscall(333);   /* io_pgetevents: not implemented */
    printf("sysprobe: syscall 333 -> %ld errno %s\n", r, errno == ENOSYS ? "ENOSYS" : "other");

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    g_a = a;
    unsigned long spin = 0;
    for (int i = 0; i < 2000000; i++) spin += (unsigned long)i * 7;
    clock_gettime(CLOCK_MONOTONIC, &b);
    g_b = b;
    long long dt = (b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
    struct timeval tv;
    gettimeofday(&tv, 0);
    time_t t = time(0);
    g_tv = tv;
    g_t = t;
    printf("sysprobe: monotonic advanced %s, realtime agrees %s, year %s\n",
           dt > 0 ? "yes" : "NO", (t - tv.tv_sec) <= 1 && (tv.tv_sec - t) <= 1 ? "yes" : "NO",
           t > 1600000000 ? "plausible" : "WRONG");

    unsigned char* buf = g_buf;
    memset(buf, 0, sizeof(g_buf));
    ssize_t n = getrandom(buf, sizeof(g_buf), 0);
    int nonzero = 0;
    for (int i = 0; i < 64; i++) nonzero += buf[i] != 0;
    tls_value = spin ^ (unsigned long)buf[0];
    printf("sysprobe: getrandom %zd bytes, %d nonzero; tls %s\n", n, nonzero,
           tls_value == (spin ^ (unsigned long)buf[0]) ? "ok" : "BAD");
    return 0;
}
