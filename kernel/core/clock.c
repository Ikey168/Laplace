/* Laplace core: the clocks programs read (#238). See include/core/clock.h. */

#include "core/clock.h"
#include "core/cpu.h"
#include "time_record.h"

static clock_base_t g_base;

/* ---- TSC frequency: count TSC ticks over 50 ms of PIT channel 2 ---- */

static uint64_t calibrate_tsc(void) {
    const uint32_t pit_hz = 1193182;
    const uint32_t count = pit_hz / 20;            /* 50 ms */
    uint8_t gate = inb(0x61);
    outb(0x61, (uint8_t)((gate & ~0x02) | 0x01));  /* speaker off, gate on */
    outb(0x43, 0xB0);                              /* channel 2, lo/hi, mode 0 */
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));
    uint64_t t0 = rdtsc();
    while (!(inb(0x61) & 0x20)) { }               /* OUT2 goes high at terminal count */
    uint64_t t1 = rdtsc();
    outb(0x61, gate);
    uint64_t hz = (t1 - t0) * 20;
    return hz ? hz : 1000000000ULL;
}

/* ---- Wall clock: the CMOS real-time clock ---- */

static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint64_t days_from_civil(int64_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint64_t)(era * 146097 + (int64_t)doe - 719468);
}

static uint64_t read_rtc_ns(void) {
    while (cmos(0x0A) & 0x80) { }                 /* update in progress */
    uint8_t sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    uint8_t day = cmos(0x07), mon = cmos(0x08), year = cmos(0x09);
    uint8_t regb = cmos(0x0B);
    if (!(regb & 0x04)) {                          /* BCD */
        #define BCD(v) (uint8_t)(((v) & 0x0F) + ((v) >> 4) * 10)
        sec = BCD(sec); min = BCD(min); day = BCD(day); mon = BCD(mon); year = BCD(year);
        hour = (uint8_t)(BCD(hour & 0x7F) | (hour & 0x80));
        #undef BCD
    }
    if (!(regb & 0x02) && (hour & 0x80)) hour = (uint8_t)(((hour & 0x7F) + 12) % 24);
    uint64_t days = days_from_civil(2000 + year, mon, day);
    uint64_t secs = days * 86400 + hour * 3600ULL + min * 60ULL + sec;
    return secs * 1000000000ULL;
}

void clock_init(void) {
    g_base.tsc_hz = calibrate_tsc();
    g_base.wall0_ns = read_rtc_ns();
    g_base.tsc0 = rdtsc();
    g_base.ns0 = 0;
}

const clock_base_t* clock_base(void) { return &g_base; }
void clock_set_base(const clock_base_t* b) { if (b->tsc_hz) g_base = *b; }

void clock_reanchor(uint64_t now_ns) {
    g_base.tsc_hz = calibrate_tsc();
    g_base.tsc0 = rdtsc();
    g_base.ns0 = now_ns;
}

uint64_t clock_tsc_to_ns(uint64_t tsc) {
    uint64_t d = tsc >= g_base.tsc0 ? tsc - g_base.tsc0 : 0;
    return g_base.ns0 + (d / g_base.tsc_hz) * 1000000000ULL +
           ((d % g_base.tsc_hz) * 1000000000ULL) / g_base.tsc_hz;
}

uint64_t clock_now_unrecorded(void) { return clock_tsc_to_ns(rdtsc()); }

uint64_t clock_monotonic_ns(void) { return clock_tsc_to_ns(ktime_read()); }
uint64_t clock_realtime_ns(void) { return g_base.wall0_ns + clock_tsc_to_ns(ktime_read()); }
