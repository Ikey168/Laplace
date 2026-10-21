/* IKOS Orthogonal Persistence - Virtualized Time Reads, kernel adapter (#163)
 *
 * Wires the pure time_record core (time_record.c) to the real x86 cycle
 * counter and exposes a global instance. Kernel code that needs a time or
 * cycle value should call ktime_read() instead of a raw RDTSC, so the value is
 * captured on a record run and reproduced on replay.
 *
 * This is the "trap or wrap" site named in #163: today IKOS has no live RDTSC
 * read (numa_allocator.c's get_rdtsc is a stub), so ktime_read() is the single
 * gate future callers route through.
 */

#include "time_record.h"
#include "core/kio.h"

/* Read the x86 timestamp counter. */
static uint64_t ktime_rdtsc(void* ctx) {
    (void)ctx;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint64_t     g_ktime_log[KTIME_LOG_MAX];
static time_record_t g_ktime;

void ktime_init(void) {
    time_record_init(&g_ktime, TIME_REC_OFF, ktime_rdtsc, 0,
                     g_ktime_log, KTIME_LOG_MAX);
}

/* Clock reads are inputs like any other: the generic journal records them
 * (#242). The time_record module stays for its host tests and the OFF mode. */
static uint64_t live_rdtsc(void) { return ktime_rdtsc(0); }

uint64_t ktime_read(void) {
    return kio_time(live_rdtsc);
}

void ktime_set_mode(time_rec_mode_t mode) {
    time_record_set_mode(&g_ktime, mode);
}

void ktime_begin_epoch(uint64_t epoch) {
    time_record_begin_epoch(&g_ktime, epoch);
}

uint32_t ktime_values(const uint64_t** out) {
    return time_record_values(&g_ktime, out);
}

int ktime_load(uint64_t epoch, const uint64_t* vals, uint32_t n) {
    return time_record_load(&g_ktime, epoch, vals, n);
}
