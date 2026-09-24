/* IKOS Orthogonal Persistence - GDB serial transport core (#198)
 *
 * See include/gdb_serial.h. Pure and host-testable: byte I/O and the serve step
 * are injected, so this file has no allocator, hardware, or serial dependency.
 * It implements just the RSP handshake: read a framed packet (skipping stray
 * acks), ack it, serve it, and send the reply with retransmit-on-'-'.
 */

#include "gdb_serial.h"

/* Stop reply for a Ctrl-C interrupt: "$S05#" + checksum of "S05".
 * 'S'(0x53) + '0'(0x30) + '5'(0x35) = 0xB8. */
static const char GDB_STOP_FRAME[] = "$S05#b8";

int gdb_serial_read_packet(const gdb_serial_ops_t* ops, char* buf, uint32_t cap) {
    if (!ops || !ops->get_byte || !buf || cap < 4) return GDB_SERIAL_ERR;

    /* Skip anything until a packet start '$', honouring an interrupt. */
    for (;;) {
        int c = ops->get_byte(ops->ctx);
        if (c < 0) return GDB_SERIAL_CLOSED;
        if (c == 0x03) return GDB_SERIAL_INTERRUPT; /* Ctrl-C between packets */
        if (c == '$') break;
        /* stray '+', '-', or noise: ignore */
    }

    uint32_t n = 0;
    buf[n++] = '$';

    /* Payload up to and including the '#' delimiter. */
    for (;;) {
        int c = ops->get_byte(ops->ctx);
        if (c < 0) return GDB_SERIAL_CLOSED;
        if (n + 3 >= cap) return GDB_SERIAL_ERR; /* need room for byte + "hh" */
        buf[n++] = (char)c;
        if (c == '#') break;
    }

    /* Two checksum hex digits. */
    int h1 = ops->get_byte(ops->ctx);
    int h2 = ops->get_byte(ops->ctx);
    if (h1 < 0 || h2 < 0) return GDB_SERIAL_CLOSED;
    buf[n++] = (char)h1;
    buf[n++] = (char)h2;

    return (int)n;
}

int gdb_serial_send_packet(const gdb_serial_ops_t* ops, const char* frame,
                           uint32_t flen) {
    if (!ops || !ops->put_byte || !ops->get_byte || !frame) return GDB_SERIAL_ERR;

    for (int attempt = 0; attempt < 4; attempt++) {
        for (uint32_t i = 0; i < flen; i++) {
            if (ops->put_byte(ops->ctx, (uint8_t)frame[i]) != 0) {
                return GDB_SERIAL_ERR;
            }
        }
        int ack = ops->get_byte(ops->ctx);
        if (ack < 0) return GDB_SERIAL_CLOSED;
        if (ack == '+') return GDB_SERIAL_OK;
        /* '-' (or anything else): retransmit */
    }
    return GDB_SERIAL_ERR;
}

static uint32_t cstr_len(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

int gdb_serial_serve_once(const gdb_serial_ops_t* ops, gdb_serial_serve_fn serve) {
    if (!ops || !serve) return GDB_SERIAL_ERR;

    /* Static: a register file or memory dump fills several kilobytes. */
    static char frame[GDB_SERIAL_MAX_FRAME];
    int flen = gdb_serial_read_packet(ops, frame, sizeof(frame));
    if (flen == GDB_SERIAL_CLOSED) return GDB_SERIAL_CLOSED;
    if (flen == GDB_SERIAL_INTERRUPT) {
        /* No packet to ack; report the target stopped. */
        return gdb_serial_send_packet(ops, GDB_STOP_FRAME, cstr_len(GDB_STOP_FRAME));
    }
    if (flen < 0) return GDB_SERIAL_ERR;

    /* Ack the well-formed request. */
    if (ops->put_byte(ops->ctx, '+') != 0) return GDB_SERIAL_ERR;

    static char reply[GDB_SERIAL_MAX_FRAME];
    int rlen = serve(frame, (uint32_t)flen, reply, sizeof(reply));
    if (rlen < 0) return GDB_SERIAL_ERR;
    if (rlen == 0) return GDB_SERIAL_OK;   /* the request expects no reply (k) */
    return gdb_serial_send_packet(ops, reply, (uint32_t)rlen);
}

int gdb_serial_loop(const gdb_serial_ops_t* ops, gdb_serial_serve_fn serve) {
    for (;;) {
        int rc = gdb_serial_serve_once(ops, serve);
        if (rc == GDB_SERIAL_OK) continue;
        return rc; /* CLOSED on a clean disconnect, or the first error */
    }
}
