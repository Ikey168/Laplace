/* Smoke-test program (#220): prints a few lines, yielding between them, so two
 * copies interleave on the console. */

#include "ulib.h"

int main(void) {
    long pid = sys_getpid();
    for (int i = 0; i < 5; i++) {
        u_puts("hello from pid ");
        u_put_u64((uint64_t)pid);
        u_puts(" line ");
        u_put_u64((uint64_t)i);
        u_puts("\n");
        sys_yield();
    }
    return 0;
}
