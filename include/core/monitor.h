/* Laplace core: the debug monitor (#226). When the machine stops it serves
 * the gdb remote protocol on COM2 and MCP (newline-delimited JSON-RPC) on COM3,
 * one request at a time, until a front end resumes the machine. */

#ifndef CORE_MONITOR_H
#define CORE_MONITOR_H

void monitor_init(void);
void monitor_run(void) __attribute__((noreturn));

#endif /* CORE_MONITOR_H */
