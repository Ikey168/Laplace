/* Laplace core: the debug monitor (#226).
 *
 * When the machine stops it serves the gdb remote protocol on COM2 and MCP
 * (newline-delimited JSON-RPC) on COM3, one request at a time, until a front
 * end resumes the machine. Both front ends drive the same machine and the same
 * position. */

#ifndef CORE_MONITOR_H
#define CORE_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

void monitor_init(void);
void monitor_run(void) __attribute__((noreturn));

/* The machine stopped live (at boot, or after a live run): record why. */
void monitor_live_stopped(void);

/* Leave recorded history (replaying to its end first, if needed) and run the
 * machine live, recording, until it stops again; then seal the recording.
 * Returns 0 once the machine stopped. */
int monitor_run_live(void);

/* A front end moved through history: the position is no longer the live stop. */
void monitor_note_navigation(void);

/* The gdb signal of the live stop while the position is still that stop (6
 * abort for a failed assertion, 11/4/8 for faults, 2 for an interrupt), or 0. */
int monitor_live_signal(void);
/* Why the machine last stopped live, as text (never NULL). */
const char* monitor_stop_text(void);

/* "monitor <cmd>" from gdb (also used by tests): write the reply text. */
int monitor_command(const char* cmd, char* out, uint32_t cap);

#endif /* CORE_MONITOR_H */
