/* IKOS Orthogonal Persistence - GDB Reverse-Debugging Stub (#172, epic #159)
 *
 * A GDB Remote Serial Protocol (RSP) stub that exposes IKOS's reverse execution
 * (#170) to a normal gdb session. GDB drives reverse debugging with two packets:
 *   - "bs": reverse single-step
 *   - "bc": reverse continue
 * which gdb only sends after the target advertises ReverseStep+ / ReverseContinue+
 * in its qSupported reply. This stub advertises them and maps bs/bc onto the
 * reverse engine, so `reverse-step` and `reverse-continue` at the gdb prompt walk
 * IKOS backward through its recorded history.
 *
 * Note this is distinct from QEMU's own gdb server (`make debug`, qemu -s): that
 * debugs the emulated CPU forward. IKOS's reverse is a property of its replay
 * engine, so gdb connects to this in-kernel stub over the serial port instead.
 * See docs/testing/reverse-debugging.md.
 *
 * The core is pure and host-testable: RSP framing (checksum, ack) and packet
 * dispatch are here, and the reverse operations are injected. The kernel adapter
 * (gdbstub_sync.c) wires bs/bc to kreverse_step / kreverse_continue and the
 * transport to the serial port.
 */

#ifndef GDBSTUB_H
#define GDBSTUB_H

#include <stdint.h>
#include <stdbool.h>

/* Operations the stub drives. The reverse pair comes first (#172); the rest
 * (#227) let a stock gdb attach: registers, memory, threads (one per process),
 * forward stepping through the recording, and breakpoints. Every pointer after
 * ctx is optional; a NULL operation makes its packet unsupported. Execution
 * operations return 0 on success; the target is reported stopped either way
 * (stop_reply describes where). */
typedef struct {
    int (*reverse_step)(void* ctx);
    int (*reverse_continue)(void* ctx);
    void* ctx;

    int (*step)(void* ctx);                  /* one instruction forward */
    int (*cont)(void* ctx);                  /* run forward to a stop */
    /* Registers of thread `tid` in gdb's amd64 'g' layout (GDBSTUB_REGS_BYTES). */
    int (*read_regs)(void* ctx, uint32_t tid, uint8_t* buf);
    /* Read `len` bytes at `addr` in thread `tid`'s address space; 0 on success. */
    int (*read_mem)(void* ctx, uint32_t tid, uint64_t addr, uint8_t* buf, uint32_t len);
    /* Thread ids in order; returns the count. */
    uint32_t (*threads)(void* ctx, uint32_t* tids, uint32_t max);
    uint32_t (*current_thread)(void* ctx);
    /* Thread display name (NUL-terminated) into out; returns its length. */
    int (*thread_name)(void* ctx, uint32_t tid, char* out, uint32_t cap);
    /* Z/z packets: type 0 software, 1 hardware, 2 write, 3 read, 4 access
     * watchpoint. 0 on success, -1 on error, -2 when unsupported. */
    int (*breakpoint)(void* ctx, int type, uint64_t addr, uint32_t kind, bool insert);
    /* The stop reply for the current state ("T05thread:1;..."); returns its
     * length. Without it the stub answers "S05". */
    int (*stop_reply)(void* ctx, char* out, uint32_t cap);
    /* qRcmd ("monitor <cmd>" in gdb): write the text reply; returns its length. */
    int (*monitor)(void* ctx, const char* cmd, char* out, uint32_t cap);
} gdbstub_ops_t;

/* gdb's amd64 register file without a target description: rax..r15, rip
 * (8 bytes each), eflags, cs, ss, ds, es, fs, gs (4 each), st0-7 (10 each),
 * eight x87 control registers (4 each), xmm0-15 (16 each), mxcsr (4). */
#define GDBSTUB_NUM_REGS    57
#define GDBSTUB_REGS_BYTES  536

/* Largest packet payload the stub accepts or produces (qSupported PacketSize). */
#define GDBSTUB_PACKET_MAX  4096

/* Per-connection state: the thread selected by Hg/Hc. */
typedef struct {
    uint32_t gthread;   /* 0: the current thread */
} gdbstub_session_t;

/* gdbstub_handle's reply meaning "send nothing" (for 'k'). */
#define GDBSTUB_NO_REPLY    (-2)

/* RSP checksum: the 8-bit sum of the payload bytes. */
uint8_t gdbstub_checksum(const char* data, uint32_t len);

/* Frame a payload as "$<payload>#<cc>". Returns the framed length, or -1 if it
 * does not fit in `out`. */
int gdbstub_frame(const char* payload, uint32_t plen, char* out, uint32_t outcap);

/* Parse a framed packet "$<payload>#<cc>": copies the payload into `out` and
 * sets *ok to whether the checksum matched. Returns the payload length, or -1 on
 * a malformed frame. */
int gdbstub_unframe(const char* frame, uint32_t flen, char* out, uint32_t outcap,
                    bool* ok);

/* Handle one RSP packet payload (unframed) and produce the response payload
 * (unframed) in `out`. Recognizes qSupported (advertising the reverse packets),
 * "?", "bs" (reverse-step), and "bc" (reverse-continue); any other packet gets
 * an empty response, which gdb reads as "unsupported". Returns the response
 * length, or -1 if it does not fit. */
int gdbstub_handle(const gdbstub_ops_t* ops, const char* packet, uint32_t plen,
                   char* out, uint32_t outcap);

/* As gdbstub_handle, with a session that remembers the selected thread. May
 * return GDBSTUB_NO_REPLY. */
int gdbstub_handle_session(const gdbstub_ops_t* ops, gdbstub_session_t* s,
                           const char* packet, uint32_t plen, char* out, uint32_t outcap);

/* Offset and size of register `regno` in the 'g' layout; false if out of range. */
bool gdbstub_reg_slot(uint32_t regno, uint32_t* offset, uint32_t* size);

/* ---- Kernel adapter (gdbstub_sync.c) ---- */
void gdbstub_bind_reverse(void);
/* Serve with a full target instead of the reverse-only defaults (#227); the
 * booted kernel installs its target (kernel/core/gdb_target.c). NULL restores
 * the defaults. */
void gdbstub_set_ops(const gdbstub_ops_t* ops);
/* Serve one framed request: unframe, dispatch, and write the framed reply into
 * `out`. Returns the framed reply length, or -1. */
int  gdbstub_serve(const char* frame, uint32_t flen, char* out, uint32_t outcap);

#endif /* GDBSTUB_H */
