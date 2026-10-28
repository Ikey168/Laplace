/* Laplace core: the console's input side, a minimal tty (#249).
 *
 * Bytes typed on COM1 are inputs. While the machine runs live, the timer tick
 * (and the idle loop) moves them from the UART into a buffer outside the
 * machine. At a kernel entry they are delivered into the machine's tty and
 * journaled with that step (IOREC_CONSOLE). A replay delivers the journaled
 * bytes at the same step and never reads the UART. Deliveries made while the
 * machine idles are marked, so a replay takes them at its idle point and not
 * at the entry that follows.
 *
 * Inside the machine there is a line discipline: canonical mode (a read
 * returns at most one line; erase, kill, ^D, and echo) or raw mode (ICANON
 * off). termios is reached by TCGETS/TCSETS*, and ^C sends SIGINT to every
 * process that has the console open (ISIG). The queue of bytes no reader has
 * taken, the partial line, and termios are in every keyframe. Bytes typed
 * while the machine is stopped for a debugger are dropped when it runs again.
 */

#ifndef CORE_TTY_H
#define CORE_TTY_H

#include <stdint.h>
#include <stdbool.h>
#include "core/proc.h"

#define KS_ID_TTY 9

void tty_init(void);

/* Live: UART -> the buffer outside the machine. Cheap; called every tick. */
void tty_poll_uart(void);
/* Live: bytes wait to be delivered. */
bool tty_pending(void);
/* Drop what was typed while the machine was stopped. */
void tty_discard_pending(void);

/* At step `step`: live, deliver what is pending and journal it; replay,
 * deliver one journaled delivery of this step (idle ones only with `idle`,
 * the others only without). True when something was delivered. */
bool tty_deliver(uint64_t step, bool idle);

/* A read would not block (canonical: a line or an end of file is there). */
bool tty_readable(void);
/* Read up to `len` bytes into user memory: the count (0 at ^D), or
 * -LX_EAGAIN when there is nothing to read. */
int64_t tty_read(process_t* p, uint64_t buf, uint64_t len);
/* Terminal ioctls (TCGETS, TCSETS*, TIOCGWINSZ, TIOCGPGRP, FIONREAD, ...).
 * False when `req` is not one. */
bool tty_ioctl(process_t* p, uint64_t req, uint64_t arg, int64_t* ret);

#endif /* CORE_TTY_H */
