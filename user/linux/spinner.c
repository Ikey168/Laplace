/* Programs that never enter the kernel while they compute (#245).
 *
 *   spinner compute ID N   N rounds of arithmetic over a 64 KiB table
 *   spinner memloop ID N   `incq (%rdi)` until the counter reaches N: every
 *                          iteration has the same registers, only memory moves
 *   spinner rep ID N       N copies of 64 KiB with `rep movsb`, each changing
 *                          the source a little
 *   spinner alarm N        a 5 ms interval timer, spinning on a flag until N
 *                          signals arrived: the same state, again and again
 *
 * Each prints "start" before computing and its result after; between the two
 * it makes no system call, so only a preemption inside its loop lets another
 * program print in between. */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static uint64_t g_table[8192];
static uint8_t  g_src[1 << 16], g_dst[1 << 16];
static volatile uint64_t g_counter;
static volatile int g_alarms;

static void on_alarm(int sig) {
    (void)sig;
    g_alarms++;
}

static uint64_t compute(uint64_t n) {
    uint64_t x = 0x9E3779B97F4A7C15ULL;
    for (uint32_t i = 0; i < 8192; i++) g_table[i] = i * 0x100000001B3ULL;
    for (uint64_t r = 0; r < n; r++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        g_table[(x >> 33) & 8191] ^= x;
    }
    uint64_t h = 0;
    for (uint32_t i = 0; i < 8192; i++) h = h * 31 + g_table[i];
    return h;
}

static uint64_t memloop(uint64_t n) {
    g_counter = 0;
    __asm__ volatile("1: incq (%0)\n\t"
                     "cmpq %1, (%0)\n\t"
                     "jb 1b"
                     :: "r"(&g_counter), "r"(n) : "memory", "cc");
    return g_counter;
}

static uint64_t rep_copies(uint64_t n) {
    for (uint32_t i = 0; i < sizeof(g_src); i++) g_src[i] = (uint8_t)(i * 7);
    uint64_t h = 0;
    for (uint64_t r = 0; r < n; r++) {
        g_src[(r * 4099) % sizeof(g_src)] ^= (uint8_t)r;
        void* d = g_dst;
        const void* s = g_src;
        uint64_t c = sizeof(g_src);
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(c) :: "memory");
        h = h * 31 + g_dst[(r * 65537) % sizeof(g_dst)];
    }
    return h;
}

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "alarm") == 0) {
        int n = atoi(argv[2]);
        signal(SIGALRM, on_alarm);
        struct itimerval it = { { 0, 5000 }, { 0, 5000 } };
        printf("spinner: spinning for %d alarms\n", n);
        setitimer(ITIMER_REAL, &it, 0);
        /* The count the loop saw: another alarm may land before the timer
         * is stopped. */
        int seen;
        while ((seen = g_alarms) < n) {}
        struct itimerval off = { { 0, 0 }, { 0, 0 } };
        setitimer(ITIMER_REAL, &off, 0);
        printf("spinner: %d alarms arrived while spinning\n", seen);
        return 0;
    }
    if (argc < 4) {
        printf("usage: spinner compute|memloop|rep ID N | alarm N\n");
        return 2;
    }
    const char* id = argv[2];
    uint64_t n = strtoull(argv[3], 0, 10);
    printf("spinner %s: start\n", id);
    fflush(stdout);
    uint64_t r;
    if (strcmp(argv[1], "compute") == 0) r = compute(n);
    else if (strcmp(argv[1], "memloop") == 0) r = memloop(n);
    else if (strcmp(argv[1], "rep") == 0) r = rep_copies(n);
    else return 2;
    printf("spinner %s: done %llx\n", id, (unsigned long long)r);
    return 0;
}
