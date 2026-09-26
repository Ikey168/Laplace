/* IKOS Orthogonal Persistence - GDB serial transport loop (#198, epic #159)
 *
 * The RSP stub (#172) frames, checksums, and dispatches gdb packets but does no
 * I/O. This layer is the transport: a loop that reads a framed request from the
 * gdb connection, handles the +/- acknowledgements, hands the frame to the stub
 * (gdbstub_serve), and writes the framed reply, so `reverse-stepi` /
 * `reverse-continue` from a real gdb session drive IKOS's reverse engine over
 * the serial port.
 *
 * The transport core is pure and host-testable: byte I/O is injected (a
 * get_byte / put_byte pair), and the serve step is a callback matching
 * gdbstub_serve, so the whole ack/read/dispatch/reply handshake is exercised
 * with a scripted byte stream. The kernel adapter (gdb_serial_sync.c) wires the
 * byte I/O to the UART and the serve step to gdbstub_serve.
 *
 * See docs/testing/reverse-debugging.md.
 */

#ifndef GDB_SERIAL_H
#define GDB_SERIAL_H

#include <stdint.h>

/* Injected byte transport. get_byte blocks for one byte and returns 0..255, or
 * a negative value when the connection is closed / end of stream. put_byte
 * writes one byte and returns 0 on success. */
typedef struct {
    int   (*get_byte)(void* ctx);
    int   (*put_byte)(void* ctx, uint8_t b);
    void* ctx;
} gdb_serial_ops_t;

/* The serve step: turn a framed request "$<payload>#<cc>" into a framed reply.
 * Signature matches gdbstub_serve(). */
/* Largest framed packet: "$" + a GDBSTUB_PACKET_MAX payload + "#cc", with
 * slack. A serve function returning 0 sends nothing (for 'k'). */
#define GDB_SERIAL_MAX_FRAME 8200

typedef int (*gdb_serial_serve_fn)(const char* frame, uint32_t flen,
                                   char* out, uint32_t outcap);

#define GDB_SERIAL_OK          0
#define GDB_SERIAL_CLOSED     -1   /* transport reached end of stream */
#define GDB_SERIAL_INTERRUPT  -2   /* a 0x03 (Ctrl-C) arrived between packets */
#define GDB_SERIAL_ERR        -3

/* Read one framed RSP packet "$<payload>#<hh>" into `buf` (cap includes the
 * frame bytes), skipping stray '+'/'-' acks between packets. Returns the frame
 * length, GDB_SERIAL_CLOSED at end of stream, GDB_SERIAL_INTERRUPT on a Ctrl-C,
 * or GDB_SERIAL_ERR (buffer too small / malformed). */
int gdb_serial_read_packet(const gdb_serial_ops_t* ops, char* buf, uint32_t cap);

/* Write the framed reply and wait for gdb's '+' ack, retransmitting on '-' (up
 * to a small bound). Returns GDB_SERIAL_OK, GDB_SERIAL_CLOSED, or GDB_SERIAL_ERR. */
int gdb_serial_send_packet(const gdb_serial_ops_t* ops, const char* frame,
                           uint32_t flen);

/* Serve one request: read a packet, ack it with '+', run `serve` to build the
 * reply, and send the reply with ack handling. A Ctrl-C interrupt is answered
 * with a stop reply. Returns GDB_SERIAL_OK, GDB_SERIAL_CLOSED, or an error. */
int gdb_serial_serve_once(const gdb_serial_ops_t* ops, gdb_serial_serve_fn serve);

/* Serve requests until the transport closes. Returns the closing status
 * (GDB_SERIAL_CLOSED on a clean disconnect, or the first error). */
int gdb_serial_loop(const gdb_serial_ops_t* ops, gdb_serial_serve_fn serve);

/* In the booted kernel the debug monitor (kernel/core/monitor.c) owns the
 * transport: it serves gdb_serial_serve_once() against gdbstub_serve on COM2
 * whenever the machine is stopped (#226). */

#endif /* GDB_SERIAL_H */
