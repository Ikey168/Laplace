/* IKOS Orthogonal Persistence - MCP stdio JSON-RPC server loop (#199, epic #159)
 *
 * The MCP tool surface (#188) parses a JSON-RPC request and dispatches it, but
 * does no I/O. This layer is the transport: a loop that reads one
 * newline-delimited JSON-RPC request, hands it to mcp_handle() (wired to the
 * kernel's time-travel ops), and writes the newline-terminated response. That
 * lets a real MCP client (an AI agent) drive record / rewind / reverse execution
 * against a running IKOS over stdio or the serial port.
 *
 * The transport core is pure and host-testable: byte I/O is injected (a
 * get_byte / put_byte pair) and the handle step is a callback, so the whole
 * read-line / dispatch / write-line loop is exercised with a scripted byte
 * stream. The kernel adapter (mcp_server_sync.c) wires the byte I/O to the UART,
 * the handle step to mcp_handle() over mcp_kernel_ops(), and registers the
 * keyframe ring and the watch probe.
 *
 * See docs/architecture/time-travel.md (Plan 2).
 */

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <stdint.h>

/* Injected byte transport. get_byte blocks for one byte and returns 0..255, or
 * a negative value at end of stream / a closed connection. put_byte writes one
 * byte and returns 0 on success. */
typedef struct {
    int   (*get_byte)(void* ctx);
    int   (*put_byte)(void* ctx, uint8_t b);
    void* ctx;
} mcp_transport_t;

/* The handle step: turn one JSON-RPC request into a JSON-RPC response.
 * Signature matches mcp_handle() with its ops already bound. */
typedef int (*mcp_handle_fn)(const char* request, uint32_t reqlen,
                             char* out, uint32_t outcap);

/* Longest request or response line. A handler returning 0 sends nothing (a
 * JSON-RPC notification). */
#define MCP_SERVER_MAX_LINE 16384

#define MCP_SERVER_OK      0
#define MCP_SERVER_CLOSED -1   /* transport reached end of stream */
#define MCP_SERVER_ERR    -2

/* Read one newline-delimited request line into `buf` (excluding the newline;
 * a trailing '\r' is stripped), skipping leading blank lines. Returns the line
 * length, MCP_SERVER_CLOSED at end of stream, or MCP_SERVER_ERR (line too long). */
int mcp_server_read_line(const mcp_transport_t* t, char* buf, uint32_t cap);

/* Serve one request: read a line, run `handle` to build the response, and write
 * it followed by '\n'. Returns MCP_SERVER_OK, MCP_SERVER_CLOSED, or an error. */
int mcp_server_serve_once(const mcp_transport_t* t, mcp_handle_fn handle);

/* Serve requests until the transport closes. Returns the closing status
 * (MCP_SERVER_CLOSED on a clean disconnect, or the first error). */
int mcp_server_loop(const mcp_transport_t* t, mcp_handle_fn handle);

/* In the booted kernel the debug monitor (kernel/core/monitor.c) owns the
 * transport: it serves mcp_server_serve_once() against mcp_handle() with the
 * kernel's ops on COM3 whenever the machine is stopped (#226). */

#endif /* MCP_SERVER_H */
