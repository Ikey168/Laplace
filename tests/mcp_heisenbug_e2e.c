/* MCP heisenbug demo (#159 Milestone E).
 *
 * A scripted agent debugs a planted heisenbug using the MCP time-travel tools.
 * The tool calls are real JSON-RPC requests handled by mcp_handle(); the
 * operations are backed by the real rewind (#169) / reverse (#170) / reverse
 * watchpoint (#171) stack over a keyframe ring (#168). A watched value is 0
 * throughout the run except that a bug flips it to 0xBAD at one position; the
 * agent finds that write by rewinding, without knowing where it was.
 *
 * The agent's steps:
 *   1. tools/list  : discover the tools.
 *   2. list_checkpoints : learn the retained rewind window.
 *   3. watch_last_write : find where the watched value last changed (the bug).
 *   4. rewind_to   : land just before the write and confirm the value was good.
 *
 * Exits non-zero if the agent fails to localize the bug, so it gates CI.
 *
 * Build: gcc -Iinclude -o mcp_heisenbug mcp_heisenbug_e2e.c kernel/mcp.c \
 *          kernel/reverse.c kernel/rewind.c kernel/keyframe_ring.c \
 *          kernel/replay_engine.c
 */

#include <stdint.h>
#include <stdbool.h>
extern int printf(const char*, ...);

#include "mcp.h"
#include "revbreak.h"  /* reverse_watchpoint; pulls reverse.h, rewind.h, ... */

#define STEPS 4
/* The planted bug: the watched value flips to 0xBAD at this position. */
#define BUG_EPOCH  2
#define BUG_OFFSET 1

/* Mock engine hooks (positions are driven by rewind; the watched value is a
 * pure function of position, below). */
typedef struct { uint64_t restored; } sim_t;
static int sim_restore(void* c, uint64_t e) { ((sim_t*)c)->restored = e; return 0; }
static int sim_load(void* c, uint64_t e) { (void)c; (void)e; return 0; }
static int sim_run(void* c, uint64_t e, uint64_t l) { (void)c; (void)e; (void)l; return 0; }
static uint64_t elen(void* c, uint64_t e) { (void)c; (void)e; return STEPS; }

/* The watched value: 0 everywhere until the bug write at (BUG_EPOCH,BUG_OFFSET),
 * 0xBAD at or after it. */
static uint64_t watched_at(reverse_pos_t p) {
    bool at_or_after = p.epoch > BUG_EPOCH ||
                       (p.epoch == BUG_EPOCH && p.offset >= BUG_OFFSET);
    return at_or_after ? 0xBADull : 0ull;
}
static uint64_t probe(void* c) { return watched_at(reverse_position((reverse_ctx_t*)c)); }

/* MCP ops backed by the real stack. */
typedef struct { keyframe_ring_t* ring; rewind_ctx_t* rw; reverse_ctx_t* rv; } td_t;
static int op_list(void* c, uint64_t* o, uint64_t* n, uint32_t* cnt) {
    td_t* t = (td_t*)c;
    if (!keyframe_ring_oldest(t->ring, o)) return -1;
    keyframe_ring_newest(t->ring, n);
    *cnt = keyframe_ring_count(t->ring);
    return 0;
}
static int op_rewind(void* c, uint64_t e, uint64_t off) {
    td_t* t = (td_t*)c;
    if (rewind_to(t->rw, e, off) != REWIND_OK) return -1;
    reverse_set_position(t->rv, e, off);
    return 0;
}
static int op_step(void* c, uint64_t* e, uint64_t* o) {
    td_t* t = (td_t*)c;
    int rc = reverse_step(t->rv);
    reverse_pos_t p = reverse_position(t->rv);
    *e = p.epoch; *o = p.offset;
    return rc == REVERSE_OK ? 0 : -1;
}
static int op_watch(void* c, uint64_t* e, uint64_t* o) {
    td_t* t = (td_t*)c;
    reverse_pos_t hit;
    if (reverse_watchpoint(t->rv, probe, t->rv, &hit) != REVBREAK_OK) return -1;
    *e = hit.epoch; *o = hit.offset;
    return 0;
}

/* Parse "key=<digits>" out of a response text (what the agent reads back). */
static bool read_after(const char* s, uint32_t n, const char* key, uint64_t* out) {
    uint32_t klen = 0; while (key[klen]) klen++;
    for (uint32_t i = 0; i + klen <= n; i++) {
        bool m = true;
        for (uint32_t j = 0; j < klen; j++) if (s[i + j] != key[j]) { m = false; break; }
        if (!m) continue;
        uint32_t p = i + klen; uint64_t v = 0; bool any = false;
        while (p < n && s[p] >= '0' && s[p] <= '9') { v = v * 10 + (uint64_t)(s[p] - '0'); p++; any = true; }
        if (any) { *out = v; return true; }
    }
    return false;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

int main(void) {
    printf("=== MCP heisenbug demo: an agent rewinds to find the bug ===\n");

    /* Set up the recorded run: keyframes 0..3, the stack, and the bad end state. */
    keyframe_ring_t ring; keyframe_ring_init(&ring, 4);
    for (uint64_t e = 0; e < 4; e++) keyframe_ring_advance(&ring, e);
    sim_t s = {0};
    replay_hooks_t h = { sim_restore, sim_load, sim_run, &s };
    replay_engine_t re; replay_init(&re, &h);
    rewind_ctx_t rw; rewind_init(&rw, &ring, &re);
    reverse_ctx_t rv; reverse_init(&rv, &rw, elen, 0);
    reverse_set_position(&rv, 3, STEPS - 1);   /* the run ended here, value = 0xBAD */

    td_t td = { &ring, &rw, &rv };
    mcp_ops_t ops = { .list_checkpoints = op_list, .rewind_to = op_rewind,
                      .reverse_step = op_step, .watch_last_write = op_watch, .ctx = &td };
    static char out[8192];   /* tools/list carries input schemas (#229) */

    /* 1. Discover the tools. */
    {
        const char* req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
        int n = mcp_handle(&ops, req, (uint32_t)0 + __builtin_strlen(req), out, sizeof(out));
        printf("  agent -> tools/list\n");
        CHECK(n > 0, "tools/list returned a schema");
    }

    /* 2. Learn the rewind window. */
    {
        const char* req = "{\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"list_checkpoints\"}}";
        int n = mcp_handle(&ops, req, (uint32_t)__builtin_strlen(req), out, sizeof(out));
        out[n < (int)sizeof(out) ? n : (int)sizeof(out) - 1] = 0;
        printf("  agent -> list_checkpoints : %s\n", out);
    }

    /* 3. The run ended badly: find where the watched value was last written. */
    uint64_t bug_epoch = 0, bug_offset = 0;
    {
        const char* req = "{\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"watch_last_write\"}}";
        int n = mcp_handle(&ops, req, (uint32_t)__builtin_strlen(req), out, sizeof(out));
        printf("  agent -> watch_last_write : ");
        for (int i = 0; i < n; i++) printf("%c", out[i]);
        printf("\n");
        bool a = read_after(out, (uint32_t)n, "epoch=", &bug_epoch);
        bool b = read_after(out, (uint32_t)n, "offset=", &bug_offset);
        CHECK(a && b && bug_epoch == BUG_EPOCH && bug_offset == BUG_OFFSET,
              "agent localized the last write to the bug position (2,1)");
    }

    /* 4. Rewind to just before the bad write and confirm the value was good. */
    {
        char req[160];
        /* build rewind_to {epoch: bug_epoch, offset: bug_offset-1 (or 0)} */
        uint64_t insp_off = bug_offset > 0 ? bug_offset - 1 : 0;
        /* the agent inspects the moment before the write */
        int m = 0;
        const char* pre = "{\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"rewind_to\",\"arguments\":{\"epoch\":";
        for (const char* p = pre; *p; p++) req[m++] = *p;
        req[m++] = (char)('0' + (bug_epoch % 10));
        const char* mid = ",\"offset\":";
        for (const char* p = mid; *p; p++) req[m++] = *p;
        req[m++] = (char)('0' + (insp_off % 10));
        const char* end = "}}}";
        for (const char* p = end; *p; p++) req[m++] = *p;
        req[m] = 0;
        int n = mcp_handle(&ops, req, (uint32_t)m, out, sizeof(out));
        printf("  agent -> rewind_to (before the write) : ");
        for (int i = 0; i < n; i++) printf("%c", out[i]);
        printf("\n");

        reverse_pos_t before = { bug_epoch, insp_off };
        reverse_pos_t at     = { bug_epoch, bug_offset };
        CHECK(watched_at(before) == 0 && watched_at(at) == 0xBAD,
              "the watched value is good just before the write and 0xBAD at it");
    }

    if (failures == 0) {
        printf("PASSED: the agent rewound to localize the planted heisenbug\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
