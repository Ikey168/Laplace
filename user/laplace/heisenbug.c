/* The planted heisenbug (#230).
 *
 * A producer fills a ring of job payloads. When it falls behind (it was
 * preempted between two clock reads, so it catches up in a "burst"), it hashes
 * the payload to pick a slot, with an off-by-one: the hash is taken modulo
 * SLOTS + 1, so about one burst write in 65 lands one past the end of the ring
 * and clobbers `batch_limit`, which sits right after it and is otherwise
 * written only once, at startup. Every 64 iterations the producer checks its
 * configuration, notices, and stops the machine.
 *
 * Whether it fires depends on three nondeterministic inputs at once: where the
 * scheduler preempted the process (the burst), the clock values (the lateness
 * test), and the random payload. Rerun it and the crash moves or disappears;
 * record it once and every moment of the failing run can be revisited: ask for
 * the last write to batch_limit and you land on the out-of-bounds store.
 */

#include "ulib.h"

#define SLOTS          64
#define BATCH_LIMIT    48
#define LATE_CYCLES    200000ULL   /* a gap this long between clock reads: we were preempted */

struct ledger {
    uint32_t ring[SLOTS];
    uint32_t batch_limit;   /* written once at startup; the overflow victim */
    uint32_t head;
    uint64_t produced;
    uint64_t bursts;
};

struct ledger g;

static void produce(uint32_t payload, int burst) {
    uint32_t slot;
    if (burst) {
        g.bursts++;
        slot = payload % (SLOTS + 1);   /* BUG: should be % SLOTS */
    } else {
        slot = g.head % SLOTS;
    }
    g.ring[slot] = payload;
    g.head++;
    g.produced++;
}

int main(void) {
    g.batch_limit = BATCH_LIMIT;

    u_puts("heisenbug: ledger at ");
    u_put_hex((uint64_t)&g);
    u_puts(", batch_limit at ");
    u_put_hex((uint64_t)&g.batch_limit);
    u_puts("\n");

    uint64_t prev = sys_time();
    for (uint64_t iter = 1;; iter++) {
        uint32_t payload;
        sys_random(&payload, sizeof(payload));
        payload &= 0xFFFF;

        uint64_t now = sys_time();
        int burst = (now - prev) > LATE_CYCLES;
        prev = now;

        produce(payload, burst);

        if (iter % 64 == 0) {
            /* Periodic configuration check: the clobber shows up here, up to
             * 63 iterations after it happened. */
            u_assert(g.batch_limit == BATCH_LIMIT, 0xBAD);
        }
        if (iter % 4096 == 0) {
            u_puts("heisenbug: iteration ");
            u_put_u64(iter);
            u_puts(", bursts ");
            u_put_u64(g.bursts);
            u_puts("\n");
        }
    }
}
