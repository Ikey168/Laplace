/* IKOS Orthogonal Persistence - MCP stdio JSON-RPC server loop core (#199)
 *
 * See include/mcp_server.h. Pure and host-testable: byte I/O and the handle
 * step are injected, so this file has no allocator, hardware, or MCP-ops
 * dependency. It implements just the newline-delimited transport: read a request
 * line, hand it to the handler, and write the response line.
 */

#include "mcp_server.h"

int mcp_server_read_line(const mcp_transport_t* t, char* buf, uint32_t cap) {
    if (!t || !t->get_byte || !buf || cap < 2) return MCP_SERVER_ERR;

    uint32_t n = 0;
    for (;;) {
        int c = t->get_byte(t->ctx);
        if (c < 0) {
            /* End of stream: a partial line is discarded; a clean boundary ends
             * the session. */
            return n > 0 ? MCP_SERVER_CLOSED : MCP_SERVER_CLOSED;
        }
        if (c == '\n') {
            if (n == 0) continue;      /* skip blank lines between requests */
            /* Strip a trailing CR from a CRLF terminator. */
            if (n > 0 && buf[n - 1] == '\r') n--;
            if (n == 0) continue;
            buf[n] = '\0';
            return (int)n;
        }
        if (n + 1 >= cap) return MCP_SERVER_ERR; /* line too long for buf */
        buf[n++] = (char)c;
    }
}

int mcp_server_serve_once(const mcp_transport_t* t, mcp_handle_fn handle) {
    if (!t || !handle) return MCP_SERVER_ERR;

    /* Static: an MCP client's initialize request and a tools/list reply with
     * input schemas run to several kilobytes. */
    static char req[MCP_SERVER_MAX_LINE];
    int rlen = mcp_server_read_line(t, req, sizeof(req));
    if (rlen == MCP_SERVER_CLOSED) return MCP_SERVER_CLOSED;
    if (rlen < 0) return MCP_SERVER_ERR;

    static char resp[MCP_SERVER_MAX_LINE];
    int n = handle(req, (uint32_t)rlen, resp, sizeof(resp));
    if (n < 0) return MCP_SERVER_ERR;
    if (n == 0) return MCP_SERVER_OK;   /* a notification: no response */

    for (int i = 0; i < n; i++) {
        if (t->put_byte(t->ctx, (uint8_t)resp[i]) != 0) return MCP_SERVER_ERR;
    }
    if (t->put_byte(t->ctx, (uint8_t)'\n') != 0) return MCP_SERVER_ERR;
    return MCP_SERVER_OK;
}

int mcp_server_loop(const mcp_transport_t* t, mcp_handle_fn handle) {
    for (;;) {
        int rc = mcp_server_serve_once(t, handle);
        if (rc == MCP_SERVER_OK) continue;
        return rc; /* CLOSED on a clean disconnect, or the first error */
    }
}
