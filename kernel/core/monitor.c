/* Laplace core: the debug monitor (#226). */

#include "core/monitor.h"
#include "core/cpu.h"
#include "core/console.h"

void monitor_init(void) { }

void monitor_run(void) {
    kprintf("monitor: not implemented yet\n");
    cpu_halt_forever();
}
