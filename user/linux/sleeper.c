/* Sleeping and idling (#246).
 *
 *   sleeper sleep N MS   N naps of MS milliseconds, each timed with the
 *                        monotonic clock
 *   sleeper work N       N rounds of arithmetic and system calls
 *   sleeper idle MS      one nap of MS milliseconds (with nothing else to run
 *                        the machine idles), then some work
 *
 * Results are kept in memory, so a replay that woke a process at a different
 * step, or computed something else, ends in a different state. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t g_log[64];
static volatile uint64_t g_acc;

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void nap(uint64_t ms) {
    struct timespec d = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&d, 0);
}

static void work(int rounds) {
    for (int r = 0; r < rounds; r++) {
        for (uint32_t i = 0; i < 200000; i++) g_acc += i ^ (uint64_t)r;
        g_acc += (uint64_t)getpid();
    }
}

int main(int argc, char** argv) {
    if (argc >= 4 && strcmp(argv[1], "sleep") == 0) {
        int n = atoi(argv[2]);
        uint64_t ms = (uint64_t)atoi(argv[3]);
        uint64_t worst = 0, total = 0;
        for (int i = 0; i < n && i < 64; i++) {
            uint64_t t0 = now_ms();
            nap(ms);
            uint64_t dt = now_ms() - t0;
            g_log[i] = dt;
            total += dt;
            if (dt < ms) { printf("sleeper: nap %d woke early (%llu ms)\n", i, (unsigned long long)dt); return 1; }
            if (dt > worst) worst = dt;
        }
        printf("sleeper: %d naps of %llu ms, none early, %llu ms in all\n", n,
               (unsigned long long)ms, (unsigned long long)total);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "work") == 0) {
        work(atoi(argv[2]));
        printf("sleeper: worker done (%llu)\n", (unsigned long long)g_acc);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "idle") == 0) {
        uint64_t ms = (uint64_t)atoi(argv[2]);
        printf("sleeper: idle for %llu ms\n", (unsigned long long)ms);
        uint64_t t0 = now_ms();
        nap(ms);
        g_log[0] = now_ms() - t0;
        printf("sleeper: woke after %llu ms; working\n", (unsigned long long)g_log[0]);
        work(40);
        printf("sleeper: done (%llu)\n", (unsigned long long)g_acc);
        return 0;
    }
    printf("usage: sleeper sleep N MS | work N | idle MS\n");
    return 2;
}
