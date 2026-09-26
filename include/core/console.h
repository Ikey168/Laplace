/* Laplace core: serial ports and the kernel console (#219).
 *
 * Port map (QEMU assigns -serial options in this order):
 *   COM1 0x3F8  console (kernel log, user output)
 *   COM2 0x2F8  gdb remote serial protocol
 *   COM3 0x3E8  MCP (newline-delimited JSON-RPC)
 * All I/O is polled; the kernel never enables the UART interrupts.
 */

#ifndef CORE_CONSOLE_H
#define CORE_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8

#define PORT_CONSOLE COM1
#define PORT_GDB     COM2
#define PORT_MCP     COM3

void serial_init(uint16_t port);
bool serial_present(uint16_t port);
void serial_putc(uint16_t port, char c);
bool serial_rx_ready(uint16_t port);
/* Blocking read. */
int  serial_getc(uint16_t port);

/* printf subset: %d %i %u %x %X %p %s %c %%, with l/ll length, width, and 0
 * padding. Output goes to the console port. */
void kprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char* fmt, va_list ap);
int  ksnprintf(char* buf, uint32_t cap, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void kputs(const char* s);
void console_write(const char* s, uint32_t len);

void panic(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#endif /* CORE_CONSOLE_H */
