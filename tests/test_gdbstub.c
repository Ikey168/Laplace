/* Host-side unit test for the GDB reverse-debugging stub (#172).
 *
 * Verifies:
 *   1. RSP framing: checksum, frame, and unframe round-trip; a bad checksum is
 *      flagged.
 *   2. qSupported advertises ReverseStep+ / ReverseContinue+ so gdb will send
 *      the reverse packets.
 *   3. "bs" maps to reverse-step and "bc" to reverse-continue, each replying
 *      with a stop; "?" replies with a stop; unknown packets reply empty.
 *
 * Build: gcc -I../include -o test_gdbstub test_gdbstub.c ../kernel/gdbstub.c
 */

#include <stdint.h>
#include <stdbool.h>
extern int printf(const char*, ...);
extern int snprintf(char*, unsigned long, const char*, ...);
extern void* memcpy(void*, const void*, unsigned long);
extern unsigned long strlen(const char*);
extern int strcmp(const char*, const char*);
extern char* strstr(const char*, const char*);

#include "gdbstub.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

static bool streq(const char* a, uint32_t alen, const char* lit) {
    uint32_t n = 0; while (lit[n]) n++;
    if (alen != n) return false;
    for (uint32_t i = 0; i < n; i++) if (a[i] != lit[i]) return false;
    return true;
}
static bool contains(const char* hay, uint32_t hlen, const char* needle) {
    uint32_t n = 0; while (needle[n]) n++;
    if (n == 0 || hlen < n) return false;
    for (uint32_t i = 0; i + n <= hlen; i++) {
        bool m = true;
        for (uint32_t j = 0; j < n; j++) if (hay[i + j] != needle[j]) { m = false; break; }
        if (m) return true;
    }
    return false;
}

/* Mock reverse ops that record what gdb drove. */
typedef struct { int steps; int continues; } rev_t;

/* A simulated target for the full packet set: threads 1-3, thread 2 current,
 * each thread's rax = its tid, rip = 0x10, memory mapped at 0x40001000. */
typedef struct {
    int steps, conts, rsteps, rconts;
    int bp_type; uint64_t bp_addr; uint32_t bp_kind; bool bp_insert;
    bool last_cmd_ok;
} sim_t;
static int sim_rstep(void* c) { ((sim_t*)c)->rsteps++; return 0; }
static int sim_rcont(void* c) { ((sim_t*)c)->rconts++; return 0; }
static int sim_step(void* c) { ((sim_t*)c)->steps++; return 0; }
static int sim_cont(void* c) { ((sim_t*)c)->conts++; return 0; }
static int sim_regs(void* c, uint32_t tid, uint8_t* buf) {
    (void)c;
    for (int i = 0; i < GDBSTUB_REGS_BYTES; i++) buf[i] = 0;
    buf[0] = (uint8_t)tid;       /* rax */
    buf[128] = 0x10;             /* rip */
    return 0;
}
static int sim_mem(void* c, uint32_t tid, uint64_t addr, uint8_t* buf, uint32_t len) {
    (void)c; (void)tid;
    if (addr < 0x40001000) return -1;
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(addr - 0x40001000 + i);
    return 0;
}
static uint32_t sim_threads(void* c, uint32_t* tids, uint32_t max) {
    (void)c;
    uint32_t n = 0;
    for (uint32_t t = 1; t <= 3 && n < max; t++) tids[n++] = t;
    return n;
}
static uint32_t sim_cur(void* c) { (void)c; return 2; }
static int sim_name(void* c, uint32_t tid, char* out, uint32_t cap) {
    (void)c; (void)cap;
    out[0] = 'p'; out[1] = (char)('0' + tid); out[2] = 0;
    return 2;
}
static int sim_bp(void* c, int type, uint64_t addr, uint32_t kind, bool insert) {
    sim_t* s = (sim_t*)c;
    s->bp_type = type; s->bp_addr = addr; s->bp_kind = kind; s->bp_insert = insert;
    return 0;
}
static int sim_stop(void* c, char* out, uint32_t cap) {
    (void)c; (void)cap;
    const char* r = "T05thread:2;";
    int n = 0;
    while (r[n]) { out[n] = r[n]; n++; }
    return n;
}
static int sim_monitor(void* c, const char* cmd, char* out, uint32_t cap) {
    (void)cap;
    ((sim_t*)c)->last_cmd_ok = cmd[0] == 'w' && cmd[1] == 'h' && cmd[5] == 0;
    out[0] = 'o'; out[1] = 'k'; out[2] = '\n';
    return 3;
}
/* A program image for exec-file and vFile (#252): bytes 0..299 with a
 * '#' and a '}' that must be escaped. */
static uint8_t g_img[300];
static int sim_exec(void* c, uint32_t pid, char* out, uint32_t cap) {
    (void)c; (void)cap;
    const char* p = pid == 7 ? "/other" : "/bin/prog";
    int n = 0;
    while (p[n]) { out[n] = p[n]; n++; }
    return n;
}
static int sim_open(void* c, const char* path) {
    (void)c;
    return path[0] == '/' && path[1] == 'b' ? 3 : -1;
}
static int64_t sim_size(void* c, int h) { (void)c; return h == 3 ? (int64_t)sizeof(g_img) : -1; }
static int sim_pread(void* c, int h, uint64_t off, uint8_t* buf, uint32_t len) {
    (void)c;
    if (h != 3) return -1;
    uint32_t n = 0;
    while (n < len && off + n < sizeof(g_img)) { buf[n] = g_img[off + n]; n++; }
    return (int)n;
}
static int g_closed = -1;
static void sim_close(void* c, int h) { (void)c; g_closed = h; }

static int mock_step(void* c) { ((rev_t*)c)->steps++; return 0; }
static int mock_cont(void* c) { ((rev_t*)c)->continues++; return 0; }

int main(void) {
    printf("=== GDB reverse-debugging stub (#172) unit test ===\n");

    /* --- 1. Framing --- */
    {
        /* checksum("OK") = 'O'(0x4F) + 'K'(0x4B) = 0x9A */
        CHECK(gdbstub_checksum("OK", 2) == 0x9A, "checksum of OK is 0x9a");

        char frame[64];
        int n = gdbstub_frame("OK", 2, frame, sizeof(frame));
        CHECK(n == 6 && streq(frame, (uint32_t)n, "$OK#9a"), "frame builds $OK#9a");

        char payload[64]; bool ok = false;
        int p = gdbstub_unframe(frame, (uint32_t)n, payload, sizeof(payload), &ok);
        CHECK(p == 2 && ok && streq(payload, (uint32_t)p, "OK"), "unframe round-trips and validates");

        char bad[] = "$OK#00";
        gdbstub_unframe(bad, 6, payload, sizeof(payload), &ok);
        CHECK(!ok, "a wrong checksum is flagged");
    }

    /* --- 2 & 3. Packet dispatch --- */
    {
        rev_t rev = { 0, 0 };
        gdbstub_ops_t ops = { .reverse_step = mock_step, .reverse_continue = mock_cont, .ctx = &rev };
        char out[128];
        int n;

        n = gdbstub_handle(&ops, "qSupported:multiprocess+", 24, out, sizeof(out));
        CHECK(n > 0 && contains(out, (uint32_t)n, "ReverseStep+") &&
              contains(out, (uint32_t)n, "ReverseContinue+"),
              "qSupported advertises the reverse packets");

        n = gdbstub_handle(&ops, "?", 1, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "S05"), "? reports a stop");

        n = gdbstub_handle(&ops, "bs", 2, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "S05") && rev.steps == 1,
              "bs drives reverse-step and replies with a stop");

        n = gdbstub_handle(&ops, "bc", 2, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "S05") && rev.continues == 1,
              "bc drives reverse-continue and replies with a stop");

        n = gdbstub_handle(&ops, "vMustReplyEmpty", 15, out, sizeof(out));
        CHECK(n == 0, "an unsupported packet replies empty");

        CHECK(rev.steps == 1 && rev.continues == 1, "reverse ops driven exactly once each");
    }

    /* --- 4. A stock gdb session (#227): registers, memory, threads, stepping,
     *        breakpoints, monitor commands --- */
    {
        sim_t sim = { 0 };
        gdbstub_ops_t ops = {
            .reverse_step = sim_rstep, .reverse_continue = sim_rcont, .ctx = &sim,
            .step = sim_step, .cont = sim_cont, .read_regs = sim_regs, .read_mem = sim_mem,
            .threads = sim_threads, .current_thread = sim_cur, .thread_name = sim_name,
            .breakpoint = sim_bp, .stop_reply = sim_stop, .monitor = sim_monitor,
            .exec_file = sim_exec, .file_open = sim_open, .file_size = sim_size,
            .file_pread = sim_pread, .file_close = sim_close,
        };
        gdbstub_session_t ses = { 0 };
        static char out[8192];
        int n;

        n = gdbstub_handle_session(&ops, &ses, "qSupported", 10, out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "swbreak+") && contains(out, (uint32_t)n, "vContSupported+"),
              "qSupported advertises breakpoints and vCont");

        n = gdbstub_handle_session(&ops, &ses, "?", 1, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "T05thread:2;"), "? uses the target's stop reply");

        n = gdbstub_handle_session(&ops, &ses, "g", 1, out, sizeof(out));
        CHECK(n == 2 * GDBSTUB_REGS_BYTES, "g returns the whole amd64 register file");
        CHECK(n > 32 && out[0] == '0' && out[1] == '2', "rax of the current thread (2) comes first, little-endian");
        CHECK(n > 16 * 8 * 2 + 4 && out[16 * 16] == '1' && out[16 * 16 + 1] == '0', "rip is register 16");

        n = gdbstub_handle_session(&ops, &ses, "Hg3", 3, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "OK"), "Hg selects a thread");
        n = gdbstub_handle_session(&ops, &ses, "p0", 2, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "0300000000000000"), "p0 reads rax of the selected thread (3)");
        n = gdbstub_handle_session(&ops, &ses, "p11", 3, out, sizeof(out));
        CHECK(n == 8, "p17 (eflags) is 4 bytes");
        n = gdbstub_handle_session(&ops, &ses, "Hg0", 3, out, sizeof(out));

        n = gdbstub_handle_session(&ops, &ses, "m40001000,4", 11, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "00010203"), "m reads memory as hex");
        n = gdbstub_handle_session(&ops, &ses, "m10,4", 5, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "E01"), "m of unmapped memory is an error");
        n = gdbstub_handle_session(&ops, &ses, "M40001000,1:ff", 14, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "E01"), "writes to recorded history are refused");

        n = gdbstub_handle_session(&ops, &ses, "qfThreadInfo", 12, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "m1,2,3"), "one thread per process");
        n = gdbstub_handle_session(&ops, &ses, "qsThreadInfo", 12, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "l"), "thread list ends");
        n = gdbstub_handle_session(&ops, &ses, "qC", 2, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "QC2"), "qC names the current thread");
        n = gdbstub_handle_session(&ops, &ses, "T3", 2, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "OK"), "T reports a live thread");
        n = gdbstub_handle_session(&ops, &ses, "T9", 2, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "E01"), "T rejects an unknown thread");
        n = gdbstub_handle_session(&ops, &ses, "qThreadExtraInfo,1", 18, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "7031"), "thread extra info is the hex name (\"p1\")");

        n = gdbstub_handle_session(&ops, &ses, "s", 1, out, sizeof(out));
        CHECK(sim.steps == 1 && streq(out, (uint32_t)n, "T05thread:2;"), "s steps and replies with a stop");
        n = gdbstub_handle_session(&ops, &ses, "vCont;c", 7, out, sizeof(out));
        CHECK(sim.conts == 1, "vCont;c continues");
        n = gdbstub_handle_session(&ops, &ses, "vCont?", 6, out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "vCont;c;C;s;S"), "vCont? lists the actions");

        n = gdbstub_handle_session(&ops, &ses, "Z0,40001234,1", 13, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "OK") && sim.bp_type == 0 && sim.bp_addr == 0x40001234 && sim.bp_insert,
              "Z0 inserts a breakpoint");
        n = gdbstub_handle_session(&ops, &ses, "z2,40001100,4", 13, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "OK") && sim.bp_type == 2 && sim.bp_kind == 4 && !sim.bp_insert,
              "z2 removes a write watchpoint");

        /* "monitor where" -> qRcmd,7768657265 */
        n = gdbstub_handle_session(&ops, &ses, "qRcmd,7768657265", 16, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "6f6b0a"), "qRcmd runs the monitor command and returns hex text");
        CHECK(sim.last_cmd_ok, "monitor received the decoded command");

        n = gdbstub_handle_session(&ops, &ses, "k", 1, out, sizeof(out));
        CHECK(n == GDBSTUB_NO_REPLY, "k expects no reply");
        n = gdbstub_handle_session(&ops, &ses, "D", 1, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "OK"), "D detaches");

        uint32_t off, sz;
        CHECK(gdbstub_reg_slot(16, &off, &sz) && off == 128 && sz == 8, "rip at offset 128");
        CHECK(gdbstub_reg_slot(56, &off, &sz) && off == 532 && sz == 4, "mxcsr after the xmm registers");
        CHECK(gdbstub_reg_slot(58, &off, &sz) && off == 544 && sz == 8, "fs_base after orig_rax");
        CHECK(gdbstub_reg_slot(75, &off, &sz) && off == 800 && sz == 16, "ymm15h is the last register");
        CHECK(!gdbstub_reg_slot(76, &off, &sz), "no register 76");

        /* --- 5. Real programs (#252): the target description, the program
         *        itself, host I/O --- */
        n = gdbstub_handle_session(&ops, &ses, "qSupported", 10, out, sizeof(out));
        CHECK(contains(out, (uint32_t)n, "qXfer:features:read+") && contains(out, (uint32_t)n, "qXfer:exec-file:read+"),
              "qSupported offers the target description and the exec file");
        char whole[16384];
        uint32_t got = 0;
        for (;;) {
            char req[64];
            int rl = snprintf(req, sizeof(req), "qXfer:features:read:target.xml:%x,200", got);
            n = gdbstub_handle_session(&ops, &ses, req, (uint32_t)rl, out, sizeof(out));
            if (n < 1 || (out[0] != 'm' && out[0] != 'l')) break;
            memcpy(whole + got, out + 1, (unsigned long)n - 1);
            got += (uint32_t)n - 1;
            if (out[0] == 'l') break;
        }
        whole[got] = 0;
        CHECK(got == strlen(gdbstub_target_xml()) && strcmp(whole, gdbstub_target_xml()) == 0,
              "target.xml reads back whole in chunks");
        CHECK(strstr(whole, "org.gnu.gdb.i386.avx") && strstr(whole, "name=\"fs_base\"") &&
              strstr(whole, "regnum=\"60\""), "it has AVX, fs_base, and the numbering");
        n = gdbstub_handle_session(&ops, &ses, "qXfer:exec-file:read::0,fff", 27, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "l/bin/prog"), "exec-file names the current program");
        n = gdbstub_handle_session(&ops, &ses, "qXfer:exec-file:read:7:0,fff", 28, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "l/other"), "... or another process's");
        n = gdbstub_handle_session(&ops, &ses, "vFile:setfs:0", 13, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "F0"), "vFile:setfs");
        n = gdbstub_handle_session(&ops, &ses, "vFile:open:2f62696e2f70726f67,0,0", 34, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "F3"), "vFile:open /bin/prog");
        n = gdbstub_handle_session(&ops, &ses, "vFile:open:2f6e6f6e65,0,0", 25, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "F-1,2"), "a missing file is ENOENT");
        n = gdbstub_handle_session(&ops, &ses, "vFile:fstat:3", 13, out, sizeof(out));
        CHECK(n == 4 + 64 && out[0] == 'F' && (uint8_t)out[4 + 35] == 0x2C && (uint8_t)out[4 + 34] == 0x01,
              "vFile:fstat gives the size (300) big-endian");
        for (uint32_t i = 0; i < sizeof(g_img); i++) g_img[i] = (uint8_t)i;
        n = gdbstub_handle_session(&ops, &ses, "vFile:pread:3,40,10", 19, out, sizeof(out));
        /* 64 bytes from 16: '#' (0x23), '$' (0x24), and '*' (0x2a) are escaped. */
        CHECK(n == 3 + 1 + 64 + 3 && out[0] == 'F' && out[1] == '4' && out[2] == '0' && out[3] == ';',
              "vFile:pread returns 64 bytes, three of them escaped");
        bool esc = false;
        for (int i = 4; i + 1 < n; i++) esc |= out[i] == '}' && (uint8_t)out[i + 1] == (0x23 ^ 0x20);
        CHECK(esc, "'#' is sent as '}' and 0x03");
        n = gdbstub_handle_session(&ops, &ses, "vFile:pread:3,40,12c", 20, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "F0;"), "reading at the end gives 0 bytes");
        n = gdbstub_handle_session(&ops, &ses, "vFile:close:3", 13, out, sizeof(out));
        CHECK(streq(out, (uint32_t)n, "F0") && g_closed == 3, "vFile:close");
    }

    if (failures == 0) {
        printf("PASSED: gdb reverse-step / reverse-continue map to the reverse engine\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
