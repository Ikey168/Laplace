/* IKOS Orthogonal Persistence - MCP Time-Travel Interface (#159 Milestone E, #229)
 *
 * Exposes Laplace's time-travel primitives as Model Context Protocol tools, so
 * an AI agent can drive a recorded machine backward over JSON-RPC. Agents are
 * poor at exactly what this stack is good at: reproducing a nondeterministic
 * bug, stepping backward, and keeping the world stable between attempts.
 *
 * Protocol (#229): initialize (the server echoes the client's protocol version
 * and advertises tools), notifications (no reply), ping, tools/list (every tool
 * carries an inputSchema), and tools/call (a text content item; isError on
 * failure).
 *
 * Tools. A position is (epoch, step, insn): step counts kernel entries within
 * the epoch; insn 0 is the moment an entry traps into the kernel, insn 1 the
 * moment after it was handled, insn k after k-1 further user instructions.
 *   list_checkpoints   the retained keyframe window (rewind horizon)
 *   where              the current position, process, and instruction pointer
 *   rewind_to          reconstruct (epoch, offset[, insn]) by re-executing
 *   reverse_step       back one kernel entry
 *   step_forward       forward one kernel entry
 *   reverse_stepi      back one instruction
 *   stepi              forward one instruction
 *   read_memory        bytes of a process's memory at the current position
 *   get_registers      a process's registers at the current position
 *   list_processes     the processes alive at the current position
 *   watch_last_write   where (pid, addr, len) last changed, to the instruction
 *   verify_replay      re-execute every retained epoch and compare checksums
 *   resume             leave history and run live (recording) to the next stop
 *
 * The core is pure and host-testable: it parses a JSON-RPC request, dispatches
 * to injected operations, and formats the response. The kernel adapter
 * (mcp_sync.c) wires the operations to the time-travel core.
 */

#ifndef MCP_H
#define MCP_H

#include <stdint.h>
#include <stdbool.h>

/* A position report. edge: 0 none, 1 at the start of the recording, 2 at its
 * end. */
typedef struct {
    uint64_t epoch, step, insn;
    uint32_t pid;
    uint64_t rip;
    int      edge;
    const char* stop;      /* why the machine last stopped (may be NULL) */
} mcp_pos_t;

typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;
} mcp_regs_t;

typedef struct {
    uint32_t pid;
    char     name[32];
    bool     current;
} mcp_proc_t;

typedef struct {
    uint32_t epochs_checked;
    uint32_t epochs_diverged;
    uint64_t first_epoch;
    uint32_t first_component;
} mcp_verify_t;

/* Operations the tools drive. Each returns 0 on success. The first four (and
 * ctx) are the original surface; everything after ctx is optional (#229). */
typedef struct {
    int (*list_checkpoints)(void* ctx, uint64_t* oldest, uint64_t* newest, uint32_t* count);
    int (*rewind_to)(void* ctx, uint64_t epoch, uint64_t offset);
    int (*reverse_step)(void* ctx, uint64_t* out_epoch, uint64_t* out_offset);
    int (*watch_last_write)(void* ctx, uint64_t* out_epoch, uint64_t* out_offset);
    void* ctx;

    int (*where)(void* ctx, mcp_pos_t* out);
    int (*goto_insn)(void* ctx, uint64_t epoch, uint64_t step, uint64_t insn);
    /* Move one kernel entry (stepi=false) or one instruction (true); returns
     * 0, or 1 when already at the corresponding edge of the recording. */
    int (*move)(void* ctx, bool backward, bool stepi);
    int (*read_memory)(void* ctx, uint32_t pid, uint64_t addr, uint8_t* buf, uint32_t len);
    int (*get_registers)(void* ctx, uint32_t pid, mcp_regs_t* out);
    uint32_t (*list_processes)(void* ctx, mcp_proc_t* out, uint32_t max);
    /* 0 found, 1 unchanged within the window, negative on error. */
    int (*watch_memory)(void* ctx, uint32_t pid, uint64_t addr, uint32_t len);
    int (*verify)(void* ctx, mcp_verify_t* out);
    int (*resume)(void* ctx);
} mcp_ops_t;

#define MCP_MAX_READ 256

/* ---- JSON helpers (exposed for tests) ---- */

/* Find integer field "key": <digits> in a JSON object. Returns true if found. */
bool mcp_json_int(const char* json, uint32_t len, const char* key, uint64_t* out);
/* Find string field "key": "value"; copies value into out. Returns true. */
bool mcp_json_str(const char* json, uint32_t len, const char* key,
                  char* out, uint32_t outcap);
/* A number given as a JSON integer or as a string ("0x40001100" or "123"). */
bool mcp_json_num(const char* json, uint32_t len, const char* key, uint64_t* out);

/* Handle one JSON-RPC message and write the response into out. Returns the
 * response length, 0 for a notification (nothing to send), or -1 if it does
 * not fit in out. */
int mcp_handle(const mcp_ops_t* ops, const char* request, uint32_t reqlen,
               char* out, uint32_t outcap);

/* ---- Kernel adapter (mcp_sync.c) ---- */
typedef uint64_t (*mcp_probe_fn)(void* ctx);
void          mcp_bind(void);
void          mcp_set_ring(const void* keyframe_ring);   /* for list_checkpoints */
void          mcp_set_watch_probe(mcp_probe_fn probe, void* ctx);
const mcp_ops_t* mcp_kernel_ops(void);

#endif /* MCP_H */
