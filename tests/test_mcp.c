/* Host-side unit test for the MCP time-travel interface (#159 Milestone E).
 *
 * Verifies:
 *   1. The JSON scanners extract integer and string fields.
 *   2. tools/list advertises the time-travel tools.
 *   3. tools/call dispatches rewind_to / reverse_step / watch_last_write /
 *      list_checkpoints to the injected ops, echoes the request id, and reports
 *      tool errors as isError.
 *
 * Build: gcc -I../include -o test_mcp test_mcp.c ../kernel/mcp.c
 */

#include <stdint.h>
#include <stdbool.h>
extern int printf(const char*, ...);

#include "mcp.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static bool contains(const char* hay, uint32_t hlen, const char* needle) {
    uint32_t n = slen(needle);
    if (n == 0 || hlen < n) return false;
    for (uint32_t i = 0; i + n <= hlen; i++) {
        bool m = true;
        for (uint32_t j = 0; j < n; j++) if (hay[i + j] != needle[j]) { m = false; break; }
        if (m) return true;
    }
    return false;
}

/* Mock ops recording what the agent drove. */
typedef struct {
    uint64_t rewind_epoch, rewind_offset; int rewinds;
    int steps;
    int watches;
} mock_t;
static int m_list(void* c, uint64_t* o, uint64_t* n, uint32_t* cnt) {
    (void)c; *o = 20; *n = 40; *cnt = 3; return 0;
}
static int m_rewind(void* c, uint64_t e, uint64_t off) {
    mock_t* m = (mock_t*)c; m->rewind_epoch = e; m->rewind_offset = off; m->rewinds++;
    return (e <= 40) ? 0 : -1;
}
static int m_step(void* c, uint64_t* e, uint64_t* o) {
    mock_t* m = (mock_t*)c; m->steps++; *e = 30; *o = 3; return 0;
}
static int m_watch(void* c, uint64_t* e, uint64_t* o) {
    mock_t* m = (mock_t*)c; m->watches++; *e = 40; *o = 1; return 0;
}

/* A fuller mock for the #229 tools. */
typedef struct {
    int goto_insn; uint64_t goto_step; int moves; bool last_backward, last_stepi;
    uint64_t watch_addr; uint32_t watch_len; int resumes;
} full_t;
static int f_rewind(void* c, uint64_t e, uint64_t o) { (void)c; (void)e; (void)o; return 0; }
static int f_where(void* c, mcp_pos_t* p) {
    (void)c;
    p->epoch = 3; p->step = 881; p->insn = 0; p->pid = 1; p->rip = 0x400001e1; p->edge = 0; p->stop = 0;
    return 0;
}
static int f_goto(void* c, uint64_t e, uint64_t s, uint64_t i) {
    (void)e; ((full_t*)c)->goto_step = s; ((full_t*)c)->goto_insn = (int)i; return 0;
}
static int f_move(void* c, bool back, bool stepi) {
    full_t* f = (full_t*)c; f->moves++; f->last_backward = back; f->last_stepi = stepi; return 0;
}
static int f_read(void* c, uint32_t pid, uint64_t addr, uint8_t* buf, uint32_t len) {
    (void)c; (void)pid;
    if (addr != 0x40001100) return -1;
    for (uint32_t i = 0; i < len; i++) buf[i] = 0;
    buf[0] = 48;
    return 0;
}
static int f_regs(void* c, uint32_t pid, mcp_regs_t* r) {
    (void)c;
    if (pid != 1) return -1;
    uint64_t* v = &r->rax;
    for (int i = 0; i < 18; i++) v[i] = 0;
    r->rax = 7; r->rip = 0x400000b3;
    return 0;
}
static uint32_t f_procs(void* c, mcp_proc_t* out, uint32_t max) {
    (void)c; (void)max;
    out[0].pid = 1; out[0].current = true;
    out[1].pid = 2; out[1].current = false;
    const char* a = "heisenbug"; const char* b = "noise";
    int i = 0; while (a[i]) { out[0].name[i] = a[i]; i++; } out[0].name[i] = 0;
    i = 0; while (b[i]) { out[1].name[i] = b[i]; i++; } out[1].name[i] = 0;
    return 2;
}
static int f_watch(void* c, uint32_t pid, uint64_t addr, uint32_t len) {
    (void)pid; ((full_t*)c)->watch_addr = addr; ((full_t*)c)->watch_len = len; return 0;
}
static int f_verify(void* c, mcp_verify_t* v) { (void)c; v->epochs_checked = 5; v->epochs_diverged = 0; return 0; }
static int f_resume(void* c) { ((full_t*)c)->resumes++; return 0; }

int main(void) {
    printf("=== MCP time-travel interface unit test ===\n");

    /* --- 1. JSON scanners --- */
    {
        const char* j = "{\"id\":7,\"params\":{\"name\":\"rewind_to\",\"arguments\":{\"epoch\":30,\"offset\":2}}}";
        uint32_t n = slen(j);
        uint64_t v = 0; char s[32];
        CHECK(mcp_json_int(j, n, "epoch", &v) && v == 30, "int field epoch=30");
        CHECK(mcp_json_int(j, n, "offset", &v) && v == 2, "int field offset=2");
        CHECK(mcp_json_int(j, n, "id", &v) && v == 7, "int field id=7");
        CHECK(mcp_json_str(j, n, "name", s, sizeof(s)) && contains(s, slen(s), "rewind_to"),
              "string field name=rewind_to");
    }

    mock_t mock = {0};
    mcp_ops_t ops = { .list_checkpoints = m_list, .rewind_to = m_rewind,
                      .reverse_step = m_step, .watch_last_write = m_watch, .ctx = &mock };
    static char out[8192];   /* tools/list carries input schemas (#229) */

    /* --- 2. tools/list --- */
    {
        const char* req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
        int n = mcp_handle(&ops, req, slen(req), out, sizeof(out));
        CHECK(n > 0 && contains(out, (uint32_t)n, "rewind_to") &&
              contains(out, (uint32_t)n, "watch_last_write") &&
              contains(out, (uint32_t)n, "reverse_step") &&
              contains(out, (uint32_t)n, "list_checkpoints"),
              "tools/list advertises the four time-travel tools");
        CHECK(contains(out, (uint32_t)n, "\"id\":1"), "tools/list echoes the request id");
    }

    /* --- 3. tools/call dispatch --- */
    {
        const char* req = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                          "\"params\":{\"name\":\"rewind_to\",\"arguments\":{\"epoch\":35,\"offset\":2}}}";
        int n = mcp_handle(&ops, req, slen(req), out, sizeof(out));
        CHECK(mock.rewinds == 1 && mock.rewind_epoch == 35 && mock.rewind_offset == 2,
              "rewind_to dispatched with epoch=35 offset=2");
        CHECK(contains(out, (uint32_t)n, "rewound to epoch=35 offset=2") &&
              contains(out, (uint32_t)n, "\"id\":2"),
              "rewind_to result text and id echoed");
    }
    {
        const char* req = "{\"id\":3,\"method\":\"tools/call\","
                          "\"params\":{\"name\":\"watch_last_write\",\"arguments\":{}}}";
        int n = mcp_handle(&ops, req, slen(req), out, sizeof(out));
        CHECK(mock.watches == 1 && contains(out, (uint32_t)n, "last write at epoch=40 offset=1"),
              "watch_last_write returns the write position");
    }
    {
        const char* req = "{\"id\":4,\"method\":\"tools/call\","
                          "\"params\":{\"name\":\"reverse_step\",\"arguments\":{}}}";
        int n = mcp_handle(&ops, req, slen(req), out, sizeof(out));
        CHECK(mock.steps == 1 && contains(out, (uint32_t)n, "stepped back to epoch=30 offset=3"),
              "reverse_step returns the new position");
    }
    {
        const char* req = "{\"id\":5,\"method\":\"tools/call\","
                          "\"params\":{\"name\":\"list_checkpoints\",\"arguments\":{}}}";
        int n = mcp_handle(&ops, req, slen(req), out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "count=3 oldest=20 newest=40"),
              "list_checkpoints returns the retained window");
    }
    {
        const char* req = "{\"id\":6,\"method\":\"tools/call\","
                          "\"params\":{\"name\":\"no_such_tool\",\"arguments\":{}}}";
        int n = mcp_handle(&ops, req, slen(req), out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "unknown tool") && contains(out, (uint32_t)n, "isError"),
              "an unknown tool is reported as an error");
    }
    {
        const char* req = "{\"id\":8,\"method\":\"nope\"}";
        int n = mcp_handle(&ops, req, slen(req), out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "-32601"), "an unknown method returns method-not-found");
    }

    /* --- MCP protocol and the full tool set (#229) --- */
    {
        full_t f = { 0 };
        mcp_ops_t ops = {
            .list_checkpoints = m_list, .rewind_to = f_rewind, .reverse_step = m_step,
            .watch_last_write = m_watch, .ctx = &f,
            .where = f_where, .goto_insn = f_goto, .move = f_move, .read_memory = f_read,
            .get_registers = f_regs, .list_processes = f_procs, .watch_memory = f_watch,
            .verify = f_verify, .resume = f_resume,
        };
        static char out[8192];
        int n;
        const char* r;

        r = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{\"protocolVersion\":"
            "\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"claude-code\",\"version\":\"2\"}}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(n > 0 && contains(out, (uint32_t)n, "\"protocolVersion\":\"2025-06-18\"") &&
              contains(out, (uint32_t)n, "\"tools\":{}") && contains(out, (uint32_t)n, "\"name\":\"laplace\""),
              "initialize echoes the protocol version and advertises tools");

        r = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        CHECK(mcp_handle(&ops, r, slen(r), out, sizeof(out)) == 0, "a notification gets no response");

        r = "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"ping\"}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "\"id\":\"abc\"") && contains(out, (uint32_t)n, "\"result\":{}"),
              "ping answers, echoing a string id");

        r = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(n > 0 && contains(out, (uint32_t)n, "\"inputSchema\"") && contains(out, (uint32_t)n, "read_memory") &&
              contains(out, (uint32_t)n, "verify_replay") && contains(out, (uint32_t)n, "resume"),
              "tools/list carries input schemas and the full tool set");

        r = "{\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"where\",\"arguments\":{}}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "position epoch=3 offset=881 insn=0 pid=1 rip=0x400001e1"),
              "where reports the position, process, and rip");

        r = "{\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"rewind_to\","
            "\"arguments\":{\"epoch\":3,\"offset\":10,\"insn\":4}}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(f.goto_insn == 4 && f.goto_step == 10 && contains(out, (uint32_t)n, "rewound to epoch=3 offset=10"),
              "rewind_to with insn lands on an instruction");

        r = "{\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"read_memory\","
            "\"arguments\":{\"pid\":1,\"addr\":\"0x40001100\",\"len\":4}}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "addr=0x40001100 len=4 bytes=30000000 u32=48"),
              "read_memory takes a hex-string address and decodes u32");

        r = "{\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"get_registers\",\"arguments\":{\"pid\":1}}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "rip=0x400000b3") && contains(out, (uint32_t)n, "rax=0x7"),
              "get_registers lists the registers");

        r = "{\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"list_processes\"}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "pid=1 name=heisenbug current\\npid=2 name=noise"),
              "list_processes names the processes and the current one");

        r = "{\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"watch_last_write\","
            "\"arguments\":{\"pid\":1,\"addr\":1073746176,\"len\":4}}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(f.watch_addr == 0x40001100 && f.watch_len == 4 && contains(out, (uint32_t)n, "last write at epoch=3"),
              "watch_last_write with pid/addr/len searches that memory");

        r = "{\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"reverse_stepi\"}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(f.moves == 1 && f.last_backward && f.last_stepi && contains(out, (uint32_t)n, "now at epoch="),
              "reverse_stepi moves back one instruction");

        r = "{\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"verify_replay\"}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "byte-exact: epochs_checked=5 epochs_diverged=0") &&
              !contains(out, (uint32_t)n, "isError"), "verify_replay reports a clean replay");

        r = "{\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"resume\"}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(f.resumes == 1 && contains(out, (uint32_t)n, "ran live and stopped at"), "resume runs live");

        r = "{\"id\":13,\"method\":\"tools/call\",\"params\":{\"name\":\"read_memory\",\"arguments\":{\"pid\":1}}}";
        n = mcp_handle(&ops, r, slen(r), out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "isError"), "missing arguments are a tool error");
    }

    if (failures == 0) {
        printf("PASSED: MCP tools dispatch record/rewind/reverse over JSON-RPC\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
