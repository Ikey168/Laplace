/* Background load for the heisenbug demo (#230): burns CPU and enters the
 * kernel periodically, so the scheduler preempts the other processes at
 * timing-dependent points. */

#include "ulib.h"

int main(void) {
    uint64_t h = (uint64_t)sys_getpid() * 0x9E3779B97F4A7C15ULL;
    for (uint64_t i = 1;; i++) {
        h ^= h >> 31;
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= i;
        if ((i & 0x3FF) == 0) sys_getpid();   /* a kernel entry: a preemption point */
    }
}
