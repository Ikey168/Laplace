/* The persistent counter (#231). No save, no load, no serialization: the
 * counter lives in ordinary memory and survives a power cut because the whole
 * machine is checkpointed. */

#include "ulib.h"

static uint64_t counter;

int main(void) {
    for (;;) {
        counter++;
        if (counter % 20000 == 0) {
            u_puts("counter: ");
            u_put_u64(counter);
            u_puts("\n");
        }
        sys_yield();
    }
}
