/* Laplace core: the clocks programs read (#238).
 *
 * Every time a program sees is derived from a journaled time stamp counter
 * read (ktime_read) and a clock base: the TSC frequency, the TSC value at the
 * base, and the wall-clock time at the base. The base is measured at boot (PIT
 * calibration, CMOS RTC) and is part of every keyframe's kernel state, so a
 * replay, even after a reboot on another machine, computes the same times the
 * recording saw.
 */

#ifndef CORE_CLOCK_H
#define CORE_CLOCK_H

#include <stdint.h>

typedef struct {
    uint64_t tsc_hz;
    uint64_t tsc0;
    uint64_t ns0;           /* monotonic nanoseconds at tsc0 */
    uint64_t wall0_ns;      /* Unix time at monotonic 0, in nanoseconds */
} clock_base_t;

/* Calibrate the TSC against the PIT and read the RTC. */
void clock_init(void);
const clock_base_t* clock_base(void);
void clock_set_base(const clock_base_t* b);   /* keyframe restore */
/* Continue live on this machine from a restored base: keep its monotonic
 * time and wall clock, re-anchored to this boot's TSC at `now_ns`. */
void clock_reanchor(uint64_t now_ns);
/* Monotonic nanoseconds now, without journaling (keyframe bookkeeping only). */
uint64_t clock_now_unrecorded(void);

/* Monotonic nanoseconds for a TSC value. */
uint64_t clock_tsc_to_ns(uint64_t tsc);

/* The journaled clocks: one ktime_read() each. */
uint64_t clock_monotonic_ns(void);
uint64_t clock_realtime_ns(void);

#endif /* CORE_CLOCK_H */
