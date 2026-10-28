/* Threads (#251): unmodified musl pthreads.
 *
 *   threads sync N   four threads add to a counter under a mutex, N times
 *                    each; a producer hands 200 items to a consumer through a
 *                    queue guarded by a condition variable; everything is
 *                    joined
 *   threads race N   two threads add 1 to a shared counter N times each,
 *                    without atomics: a preemption between the load and the
 *                    store loses the other thread's updates
 *
 * Results are kept in memory, so a replay that interleaves the threads
 * differently ends in a different state. */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/random.h>
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static long g_counter;
static long g_iters;

static int  g_queue[8];
static int  g_head, g_tail, g_count;
static long g_consumed_sum;

/* The racy counter: volatile so every update is a load, an add, and a store. */
volatile long g_race;
static long g_tids[2];
/* Journaled inputs, kept (#256): a sabotaged replay of them ends elsewhere. */
static struct timespec g_started;
static unsigned char g_noise[8];

static void* adder(void* arg) {
    (void)arg;
    for (long i = 0; i < g_iters; i++) {
        pthread_mutex_lock(&g_lock);
        g_counter++;
        pthread_mutex_unlock(&g_lock);
    }
    return 0;
}

static void* producer(void* arg) {
    (void)arg;
    for (int i = 1; i <= 200; i++) {
        pthread_mutex_lock(&g_lock);
        while (g_count == 8) pthread_cond_wait(&g_cv, &g_lock);
        g_queue[g_tail] = i;
        g_tail = (g_tail + 1) % 8;
        g_count++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_lock);
    }
    return 0;
}

static void* consumer(void* arg) {
    (void)arg;
    for (int i = 0; i < 200; i++) {
        pthread_mutex_lock(&g_lock);
        while (g_count == 0) pthread_cond_wait(&g_cv, &g_lock);
        g_consumed_sum += g_queue[g_head];
        g_head = (g_head + 1) % 8;
        g_count--;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_lock);
    }
    return 0;
}

static void* racer(void* arg) {
    long me = (long)arg;
    g_tids[me] = syscall(SYS_gettid);
    for (long i = 0; i < g_iters; i++) {
        long v = g_race;
        for (volatile int k = 0; k < 3; k++) {}     /* widen the window */
        g_race = v + 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: threads sync|race N\n");
        return 2;
    }
    g_iters = atol(argv[2]);
    if (strcmp(argv[1], "sync") == 0) {
        pthread_t t[6];
        for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, adder, 0);
        pthread_create(&t[4], 0, producer, 0);
        pthread_create(&t[5], 0, consumer, 0);
        for (int i = 0; i < 6; i++) pthread_join(t[i], 0);
        printf("threads: 6 threads joined; counter %ld (want %ld); queue passed items summing to %ld (want 20100)\n",
               g_counter, 4 * g_iters, g_consumed_sum);
        return g_counter == 4 * g_iters && g_consumed_sum == 20100 ? 0 : 1;
    }
    if (strcmp(argv[1], "race") == 0) {
        pthread_t t[2];
        clock_gettime(CLOCK_MONOTONIC, &g_started);
        getrandom(g_noise, sizeof(g_noise), 0);
        printf("race: two threads, %ld unsynchronized increments each; the counter is at %p\n", g_iters,
               (void*)&g_race);
        fflush(stdout);
        pthread_create(&t[0], 0, racer, (void*)0);
        pthread_create(&t[1], 0, racer, (void*)1);
        pthread_join(t[0], 0);
        pthread_join(t[1], 0);
        long want = 2 * g_iters;
        printf("race: counter %ld of %ld (%ld updates lost); threads %ld and %ld\n", g_race, want,
               want - g_race, g_tids[0], g_tids[1]);
        return 0;
    }
    return 2;
}
