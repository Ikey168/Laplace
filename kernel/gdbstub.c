/* IKOS Orthogonal Persistence - GDB Reverse-Debugging Stub (#172, epic #159)
 *
 * See include/gdbstub.h and docs/testing/reverse-debugging.md.
 *
 * Pure RSP core: framing, checksum, and packet dispatch. No allocator, no
 * hardware; the reverse operations and the transport are injected/wired by the
 * adapter.
 */

#include "gdbstub.h"

static uint32_t lit_len(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static bool eq_lit(const char* p, uint32_t plen, const char* lit) {
    uint32_t n = lit_len(lit);
    if (plen != n) return false;
    for (uint32_t i = 0; i < n; i++) if (p[i] != lit[i]) return false;
    return true;
}

static bool has_prefix(const char* p, uint32_t plen, const char* lit) {
    uint32_t n = lit_len(lit);
    if (plen < n) return false;
    for (uint32_t i = 0; i < n; i++) if (p[i] != lit[i]) return false;
    return true;
}

/* Copy a NUL-terminated literal into out; returns length or -1 if it overflows. */
static int emit(char* out, uint32_t cap, const char* s) {
    uint32_t n = lit_len(s);
    if (n > cap) return -1;
    for (uint32_t i = 0; i < n; i++) out[i] = s[i];
    return (int)n;
}

static char hex_digit(int v) {
    return (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint8_t gdbstub_checksum(const char* data, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += (uint8_t)data[i];
    return (uint8_t)(sum & 0xFF);
}

int gdbstub_frame(const char* payload, uint32_t plen, char* out, uint32_t outcap) {
    uint32_t need = plen + 4; /* '$' + payload + '#' + two hex */
    if (need > outcap) return -1;
    out[0] = '$';
    for (uint32_t i = 0; i < plen; i++) out[1 + i] = payload[i];
    out[1 + plen] = '#';
    uint8_t c = gdbstub_checksum(payload, plen);
    out[2 + plen] = hex_digit((c >> 4) & 0xF);
    out[3 + plen] = hex_digit(c & 0xF);
    return (int)need;
}

int gdbstub_unframe(const char* frame, uint32_t flen, char* out, uint32_t outcap,
                    bool* ok) {
    if (ok) *ok = false;
    if (flen < 4 || frame[0] != '$') return -1;

    /* Find the '#' that ends the payload. */
    uint32_t hash = 0;
    bool found = false;
    for (uint32_t i = 1; i < flen; i++) {
        if (frame[i] == '#') { hash = i; found = true; break; }
    }
    if (!found || hash + 2 >= flen) return -1;

    uint32_t plen = hash - 1;
    if (plen > outcap) return -1;
    for (uint32_t i = 0; i < plen; i++) out[i] = frame[1 + i];

    int hi = hex_val(frame[hash + 1]);
    int lo = hex_val(frame[hash + 2]);
    if (hi < 0 || lo < 0) return -1;
    uint8_t want = (uint8_t)((hi << 4) | lo);
    if (ok) *ok = (want == gdbstub_checksum(out, plen));
    return (int)plen;
}

/* ---- Packet dispatch ---- */

/* Register layout of gdb's default amd64 description. */
bool gdbstub_reg_slot(uint32_t regno, uint32_t* offset, uint32_t* size) {
    uint32_t off, sz;
    if (regno < 17)      { off = regno * 8;                 sz = 8; }   /* rax..r15, rip */
    else if (regno < 24) { off = 136 + (regno - 17) * 4;    sz = 4; }   /* eflags, segs */
    else if (regno < 32) { off = 164 + (regno - 24) * 10;   sz = 10; }  /* st0-7 */
    else if (regno < 40) { off = 244 + (regno - 32) * 4;    sz = 4; }   /* x87 control */
    else if (regno < 56) { off = 276 + (regno - 40) * 16;   sz = 16; }  /* xmm0-15 */
    else if (regno == 56){ off = 532;                       sz = 4; }   /* mxcsr */
    else if (regno < 60) { off = 536 + (regno - 57) * 8;    sz = 8; }   /* orig_rax, fs/gs base */
    else if (regno < 76) { off = 560 + (regno - 60) * 16;   sz = 16; }  /* ymm0h-15h */
    else return false;
    if (offset) *offset = off;
    if (size) *size = sz;
    return true;
}

/* The target description (#252): amd64 for GNU/Linux with the x87, SSE,
 * segment-base, and AVX features, numbered as gdbstub_reg_slot lays them out. */
static const char g_tdesc[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\"><architecture>i386:x86-64</architecture><osabi>GNU/Linux</osabi>"
    "<feature name=\"org.gnu.gdb.i386.core\">"
    "<reg name=\"rax\" bitsize=\"64\" type=\"int64\" regnum=\"0\"/>"
    "<reg name=\"rbx\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"rcx\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"rdx\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"rsi\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"rdi\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"rbp\" bitsize=\"64\" type=\"data_ptr\"/>"
    "<reg name=\"rsp\" bitsize=\"64\" type=\"data_ptr\"/>"
    "<reg name=\"r8\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"r9\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"r10\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"r11\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"r12\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"r13\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"r14\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"r15\" bitsize=\"64\" type=\"int64\"/>"
    "<reg name=\"rip\" bitsize=\"64\" type=\"code_ptr\"/>"
    "<reg name=\"eflags\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"cs\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"ss\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"ds\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"es\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"fs\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"gs\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"st0\" bitsize=\"80\" type=\"i387_ext\"/>"
    "<reg name=\"st1\" bitsize=\"80\" type=\"i387_ext\"/>"
    "<reg name=\"st2\" bitsize=\"80\" type=\"i387_ext\"/>"
    "<reg name=\"st3\" bitsize=\"80\" type=\"i387_ext\"/>"
    "<reg name=\"st4\" bitsize=\"80\" type=\"i387_ext\"/>"
    "<reg name=\"st5\" bitsize=\"80\" type=\"i387_ext\"/>"
    "<reg name=\"st6\" bitsize=\"80\" type=\"i387_ext\"/>"
    "<reg name=\"st7\" bitsize=\"80\" type=\"i387_ext\"/>"
    "<reg name=\"fctrl\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
    "<reg name=\"fstat\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
    "<reg name=\"ftag\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
    "<reg name=\"fiseg\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
    "<reg name=\"fioff\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
    "<reg name=\"foseg\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
    "<reg name=\"fooff\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
    "<reg name=\"fop\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
    "</feature><feature name=\"org.gnu.gdb.i386.sse\">"
    "<vector id=\"v4f\" type=\"ieee_single\" count=\"4\"/>"
    "<vector id=\"v2d\" type=\"ieee_double\" count=\"2\"/>"
    "<vector id=\"v16i8\" type=\"int8\" count=\"16\"/>"
    "<vector id=\"v8i16\" type=\"int16\" count=\"8\"/>"
    "<vector id=\"v4i32\" type=\"int32\" count=\"4\"/>"
    "<vector id=\"v2i64\" type=\"int64\" count=\"2\"/>"
    "<union id=\"vec128\"><field name=\"v4_float\" type=\"v4f\"/><field name=\"v2_double\" type=\"v2d\"/>"
    "<field name=\"v16_int8\" type=\"v16i8\"/><field name=\"v8_int16\" type=\"v8i16\"/>"
    "<field name=\"v4_int32\" type=\"v4i32\"/><field name=\"v2_int64\" type=\"v2i64\"/>"
    "<field name=\"uint128\" type=\"uint128\"/></union>"
    "<reg name=\"xmm0\" bitsize=\"128\" type=\"vec128\" regnum=\"40\"/>"
    "<reg name=\"xmm1\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm2\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm3\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm4\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm5\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm6\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm7\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm8\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm9\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm10\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm11\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm12\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm13\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm14\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"xmm15\" bitsize=\"128\" type=\"vec128\"/>"
    "<reg name=\"mxcsr\" bitsize=\"32\" type=\"int\" group=\"vector\"/>"
    "</feature><feature name=\"org.gnu.gdb.i386.linux\">"
    "<reg name=\"orig_rax\" bitsize=\"64\" type=\"int\" regnum=\"57\"/>"
    "</feature><feature name=\"org.gnu.gdb.i386.segments\">"
    "<reg name=\"fs_base\" bitsize=\"64\" type=\"int\" regnum=\"58\"/>"
    "<reg name=\"gs_base\" bitsize=\"64\" type=\"int\"/>"
    "</feature><feature name=\"org.gnu.gdb.i386.avx\">"
    "<reg name=\"ymm0h\" bitsize=\"128\" type=\"uint128\" regnum=\"60\"/>"
    "<reg name=\"ymm1h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm2h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm3h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm4h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm5h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm6h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm7h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm8h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm9h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm10h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm11h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm12h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm13h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm14h\" bitsize=\"128\" type=\"uint128\"/>"
    "<reg name=\"ymm15h\" bitsize=\"128\" type=\"uint128\"/>"
    "</feature></target>";

const char* gdbstub_target_xml(void) { return g_tdesc; }

typedef struct { char* buf; uint32_t cap; uint32_t len; bool ovf; } out_t;

static void put(out_t* o, char c) {
    if (o->len < o->cap) o->buf[o->len++] = c; else o->ovf = true;
}
static void puts_(out_t* o, const char* s) { while (*s) put(o, *s++); }
static void put_hex_byte(out_t* o, uint8_t b) {
    put(o, hex_digit(b >> 4));
    put(o, hex_digit(b & 0xF));
}
static void put_hex_num(out_t* o, uint64_t v) {
    char tmp[16];
    int n = 0;
    do { tmp[n++] = hex_digit((int)(v & 0xF)); v >>= 4; } while (v);
    while (n) put(o, tmp[--n]);
}
static int done(out_t* o) { return o->ovf ? -1 : (int)o->len; }

/* Binary data in a reply: '#', '$', '}' and '*' are escaped as '}' and the
 * byte xor 0x20. */
static void put_bin(out_t* o, const uint8_t* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == '#' || c == '$' || c == '}' || c == '*') {
            put(o, '}');
            put(o, (char)(c ^ 0x20));
        } else {
            put(o, (char)c);
        }
    }
}

/* qXfer reads of a string: "m" and a chunk, or "l" and the last one. */
static int xfer_string(out_t* o, const char* s, uint64_t slen, uint64_t off, uint64_t len) {
    if (off >= slen) {
        put(o, 'l');
        return done(o);
    }
    uint64_t n = slen - off;
    if (len > GDBSTUB_PACKET_MAX / 2) len = GDBSTUB_PACKET_MAX / 2;
    put(o, n > len ? 'm' : 'l');
    if (n > len) n = len;
    put_bin(o, (const uint8_t*)s + off, (uint32_t)n);
    return done(o);
}

static void put_be32(uint8_t* b, uint32_t v) {
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
}

/* Parse a hex number at p[*i], stopping at a non-hex character. */
static bool parse_hex(const char* p, uint32_t plen, uint32_t* i, uint64_t* out) {
    uint64_t v = 0;
    uint32_t start = *i;
    while (*i < plen && hex_val(p[*i]) >= 0) {
        v = (v << 4) | (uint64_t)hex_val(p[*i]);
        (*i)++;
    }
    *out = v;
    return *i > start;
}

/* A thread id in an H/T/qThreadExtraInfo packet: hex, or -1 / 0 for "any". */
static uint32_t parse_tid(const char* p, uint32_t plen, uint32_t i) {
    if (i < plen && p[i] == '-') return 0;
    uint64_t v = 0;
    parse_hex(p, plen, &i, &v);
    return (uint32_t)v;
}

static int stop_reply(const gdbstub_ops_t* ops, char* out, uint32_t outcap) {
    if (ops && ops->stop_reply) return ops->stop_reply(ops->ctx, out, outcap);
    return emit(out, outcap, "S05");
}

static uint32_t effective_tid(const gdbstub_ops_t* ops, const gdbstub_session_t* s) {
    if (s->gthread) return s->gthread;
    return ops->current_thread ? ops->current_thread(ops->ctx) : 0;
}

int gdbstub_handle(const gdbstub_ops_t* ops, const char* packet, uint32_t plen,
                   char* out, uint32_t outcap) {
    gdbstub_session_t s = { 0 };
    int n = gdbstub_handle_session(ops, &s, packet, plen, out, outcap);
    return n == GDBSTUB_NO_REPLY ? 0 : n;
}

int gdbstub_handle_session(const gdbstub_ops_t* ops, gdbstub_session_t* s,
                           const char* packet, uint32_t plen, char* out, uint32_t outcap) {
    out_t o = { out, outcap, 0, false };

    /* Feature negotiation: advertise the reverse-execution packets so gdb will
     * send bs/bc for reverse-step / reverse-continue. */
    if (has_prefix(packet, plen, "qSupported")) {
        puts_(&o, "PacketSize=1000;ReverseStep+;ReverseContinue+");
        if (ops && ops->breakpoint) puts_(&o, ";swbreak+;hwbreak+");
        if (ops && ops->cont) puts_(&o, ";vContSupported+");
        if (ops && ops->read_regs) puts_(&o, ";qXfer:features:read+");
        if (ops && ops->exec_file) puts_(&o, ";qXfer:exec-file:read+");
        if (ops && ops->auxv) puts_(&o, ";qXfer:auxv:read+");
        if (ops && ops->libraries) puts_(&o, ";qXfer:libraries-svr4:read+");
        return done(&o);
    }

    /* Halt reason. */
    if (eq_lit(packet, plen, "?"))
        return stop_reply(ops, out, outcap);

    /* Reverse single-step. */
    if (eq_lit(packet, plen, "bs")) {
        if (ops && ops->reverse_step) ops->reverse_step(ops->ctx);
        return stop_reply(ops, out, outcap);
    }

    /* Reverse continue. */
    if (eq_lit(packet, plen, "bc")) {
        if (ops && ops->reverse_continue) ops->reverse_continue(ops->ctx);
        return stop_reply(ops, out, outcap);
    }

    if (!ops) return 0;

    /* Forward execution through the recording (and live past its end). */
    if (ops->step && (eq_lit(packet, plen, "s") || has_prefix(packet, plen, "vCont;s"))) {
        ops->step(ops->ctx);
        return stop_reply(ops, out, outcap);
    }
    if (ops->cont && (eq_lit(packet, plen, "c") || has_prefix(packet, plen, "vCont;c"))) {
        ops->cont(ops->ctx);
        return stop_reply(ops, out, outcap);
    }
    if (ops->cont && eq_lit(packet, plen, "vCont?"))
        return emit(out, outcap, "vCont;c;C;s;S");

    /* Registers. */
    if (ops->read_regs && eq_lit(packet, plen, "g")) {
        uint8_t regs[GDBSTUB_REGS_BYTES];
        if (ops->read_regs(ops->ctx, effective_tid(ops, s), regs) != 0) return emit(out, outcap, "E01");
        for (uint32_t i = 0; i < GDBSTUB_REGS_BYTES; i++) put_hex_byte(&o, regs[i]);
        return done(&o);
    }
    if (ops->read_regs && plen > 1 && packet[0] == 'p') {
        uint32_t i = 1;
        uint64_t regno;
        uint32_t off, sz;
        if (!parse_hex(packet, plen, &i, &regno) || !gdbstub_reg_slot((uint32_t)regno, &off, &sz))
            return emit(out, outcap, "E01");
        uint8_t regs[GDBSTUB_REGS_BYTES];
        if (ops->read_regs(ops->ctx, effective_tid(ops, s), regs) != 0) return emit(out, outcap, "E01");
        for (uint32_t b = 0; b < sz; b++) put_hex_byte(&o, regs[off + b]);
        return done(&o);
    }
    /* Recorded history is read-only. */
    if (plen > 0 && (packet[0] == 'G' || packet[0] == 'P' || packet[0] == 'M' || packet[0] == 'X'))
        return emit(out, outcap, "E01");

    /* Memory. */
    if (ops->read_mem && plen > 1 && packet[0] == 'm') {
        uint32_t i = 1;
        uint64_t addr, len;
        if (!parse_hex(packet, plen, &i, &addr) || i >= plen || packet[i] != ',') return emit(out, outcap, "E01");
        i++;
        if (!parse_hex(packet, plen, &i, &len)) return emit(out, outcap, "E01");
        if (len > GDBSTUB_PACKET_MAX / 2) len = GDBSTUB_PACKET_MAX / 2;
        uint8_t buf[GDBSTUB_PACKET_MAX / 2];
        if (ops->read_mem(ops->ctx, effective_tid(ops, s), addr, buf, (uint32_t)len) != 0)
            return emit(out, outcap, "E01");
        for (uint32_t b = 0; b < (uint32_t)len; b++) put_hex_byte(&o, buf[b]);
        return done(&o);
    }

    /* Threads: one per process. */
    if (plen >= 2 && packet[0] == 'H' && (packet[1] == 'g' || packet[1] == 'c')) {
        if (packet[1] == 'g') s->gthread = parse_tid(packet, plen, 2);
        return emit(out, outcap, "OK");
    }
    if (plen > 1 && packet[0] == 'T' && ops->threads) {
        uint32_t want = parse_tid(packet, plen, 1);
        uint32_t tids[64];
        uint32_t n = ops->threads(ops->ctx, tids, 64);
        for (uint32_t k = 0; k < n; k++) if (tids[k] == want) return emit(out, outcap, "OK");
        return emit(out, outcap, "E01");
    }
    if (eq_lit(packet, plen, "qC") && ops->current_thread) {
        puts_(&o, "QC");
        put_hex_num(&o, ops->current_thread(ops->ctx));
        return done(&o);
    }
    if (eq_lit(packet, plen, "qfThreadInfo") && ops->threads) {
        uint32_t tids[64];
        uint32_t n = ops->threads(ops->ctx, tids, 64);
        if (n == 0) return emit(out, outcap, "l");
        put(&o, 'm');
        for (uint32_t k = 0; k < n; k++) {
            if (k) put(&o, ',');
            put_hex_num(&o, tids[k]);
        }
        return done(&o);
    }
    if (eq_lit(packet, plen, "qsThreadInfo")) return emit(out, outcap, "l");
    if (has_prefix(packet, plen, "qThreadExtraInfo,") && ops->thread_name) {
        char name[64];
        int n = ops->thread_name(ops->ctx, parse_tid(packet, plen, 17), name, sizeof(name));
        for (int k = 0; k < n; k++) put_hex_byte(&o, (uint8_t)name[k]);
        return done(&o);
    }
    /* The target description (#252). */
    if (has_prefix(packet, plen, "qXfer:features:read:") && ops->read_regs) {
        uint32_t i = 20;
        if (!has_prefix(packet + i, plen - i, "target.xml:")) return emit(out, outcap, "E00");
        i += 11;
        uint64_t off, len;
        if (!parse_hex(packet, plen, &i, &off) || i >= plen || packet[i] != ',') return emit(out, outcap, "E01");
        i++;
        if (!parse_hex(packet, plen, &i, &len)) return emit(out, outcap, "E01");
        return xfer_string(&o, g_tdesc, sizeof(g_tdesc) - 1, off, len);
    }
    /* The auxiliary vector and the shared libraries (#254): "qXfer:auxv:read:",
     * "qXfer:libraries-svr4:read:", an annex (ignored), ':', then off,len. */
    bool want_auxv = has_prefix(packet, plen, "qXfer:auxv:read:") && ops->auxv;
    bool want_libs = has_prefix(packet, plen, "qXfer:libraries-svr4:read:") && ops->libraries;
    if (want_auxv || want_libs) {
        uint32_t i = want_auxv ? 16 : 26;
        while (i < plen && packet[i] != ':') i++;          /* annex (ignored) */
        if (i >= plen) return emit(out, outcap, "E01");
        i++;
        uint64_t off, len;
        if (!parse_hex(packet, plen, &i, &off) || i >= plen || packet[i] != ',') return emit(out, outcap, "E01");
        i++;
        if (!parse_hex(packet, plen, &i, &len)) return emit(out, outcap, "E01");
        static char doc[8192];
        int n = want_auxv ? ops->auxv(ops->ctx, 0, (uint8_t*)doc, sizeof(doc))
                          : ops->libraries(ops->ctx, 0, doc, sizeof(doc));
        if (n < 0) return emit(out, outcap, "E00");
        return xfer_string(&o, doc, (uint64_t)n, off, len);
    }
    /* The program, so gdb needs no file argument (#252). */
    if (has_prefix(packet, plen, "qXfer:exec-file:read:") && ops->exec_file) {
        uint32_t i = 21;
        uint64_t pid = 0, off, len;
        parse_hex(packet, plen, &i, &pid);
        if (i >= plen || packet[i] != ':') return emit(out, outcap, "E01");
        i++;
        if (!parse_hex(packet, plen, &i, &off) || i >= plen || packet[i] != ',') return emit(out, outcap, "E01");
        i++;
        if (!parse_hex(packet, plen, &i, &len)) return emit(out, outcap, "E01");
        char path[256];
        int n = ops->exec_file(ops->ctx, (uint32_t)pid, path, sizeof(path));
        if (n < 0) return emit(out, outcap, "E00");
        return xfer_string(&o, path, (uint64_t)n, off, len);
    }
    /* Host I/O: gdb reads the program through "target:" paths. */
    if (has_prefix(packet, plen, "vFile:")) {
        uint32_t i = 6;
        if (has_prefix(packet + i, plen - i, "setfs:")) return emit(out, outcap, "F0");
        if (has_prefix(packet + i, plen - i, "open:") && ops->file_open) {
            i += 5;
            char path[256];
            uint32_t n = 0;
            while (i + 1 < plen && packet[i] != ',' && n + 1 < sizeof(path)) {
                int hi = hex_val(packet[i]), lo = hex_val(packet[i + 1]);
                if (hi < 0 || lo < 0) break;
                path[n++] = (char)((hi << 4) | lo);
                i += 2;
            }
            path[n] = 0;
            int h = ops->file_open(ops->ctx, path);
            if (h < 0) return emit(out, outcap, "F-1,2");     /* ENOENT */
            put(&o, 'F');
            put_hex_num(&o, (uint64_t)h);
            return done(&o);
        }
        if (has_prefix(packet + i, plen - i, "pread:") && ops->file_pread) {
            i += 6;
            uint64_t h, count, off;
            if (!parse_hex(packet, plen, &i, &h) || i >= plen || packet[i++] != ',' ||
                !parse_hex(packet, plen, &i, &count) || i >= plen || packet[i++] != ',' ||
                !parse_hex(packet, plen, &i, &off)) {
                return emit(out, outcap, "F-1,16");             /* EINVAL (gdb's) */
            }
            if (count > GDBSTUB_PACKET_MAX / 2 - 16) count = GDBSTUB_PACKET_MAX / 2 - 16;
            uint8_t buf[GDBSTUB_PACKET_MAX / 2];
            int n = ops->file_pread(ops->ctx, (int)h, off, buf, (uint32_t)count);
            if (n < 0) return emit(out, outcap, "F-1,9");        /* EBADF */
            put(&o, 'F');
            put_hex_num(&o, (uint64_t)n);
            put(&o, ';');
            put_bin(&o, buf, (uint32_t)n);
            return done(&o);
        }
        if (has_prefix(packet + i, plen - i, "fstat:") && ops->file_size) {
            i += 6;
            uint64_t h;
            parse_hex(packet, plen, &i, &h);
            int64_t size = ops->file_size(ops->ctx, (int)h);
            if (size < 0) return emit(out, outcap, "F-1,9");
            /* gdb's struct stat: big-endian dev, ino, mode, nlink, uid, gid,
             * rdev (4 each), size, blksize, blocks (8 each), atime, mtime,
             * ctime (4 each). */
            uint8_t st[64];
            for (uint32_t k = 0; k < sizeof(st); k++) st[k] = 0;
            put_be32(st + 8, 0100555);                         /* a regular file */
            put_be32(st + 12, 1);
            put_be32(st + 28, (uint32_t)((uint64_t)size >> 32));
            put_be32(st + 32, (uint32_t)size);
            put_be32(st + 40, 4096);
            puts_(&o, "F40;");
            put_bin(&o, st, sizeof(st));
            return done(&o);
        }
        if (has_prefix(packet + i, plen - i, "close:") && ops->file_close) {
            i += 6;
            uint64_t h;
            parse_hex(packet, plen, &i, &h);
            ops->file_close(ops->ctx, (int)h);
            return emit(out, outcap, "F0");
        }
        return emit(out, outcap, "F-1,58");                     /* ENOSYS (gdb's) */
    }
    if (eq_lit(packet, plen, "qAttached")) return emit(out, outcap, "1");
    if (eq_lit(packet, plen, "qOffsets")) return emit(out, outcap, "Text=0;Data=0;Bss=0");
    if (has_prefix(packet, plen, "qSymbol")) return emit(out, outcap, "OK");

    /* Breakpoints and watchpoints. */
    if (ops->breakpoint && plen > 3 && (packet[0] == 'Z' || packet[0] == 'z') && packet[2] == ',') {
        int type = packet[1] - '0';
        uint32_t i = 3;
        uint64_t addr, kind = 1;
        if (type < 0 || type > 4 || !parse_hex(packet, plen, &i, &addr)) return emit(out, outcap, "E01");
        if (i < plen && packet[i] == ',') {
            i++;
            parse_hex(packet, plen, &i, &kind);
        }
        int rc = ops->breakpoint(ops->ctx, type, addr, (uint32_t)kind, packet[0] == 'Z');
        if (rc == -2) return 0;
        return emit(out, outcap, rc == 0 ? "OK" : "E01");
    }

    /* "monitor <cmd>": the command arrives hex-encoded; the reply is hex text. */
    if (has_prefix(packet, plen, "qRcmd,") && ops->monitor) {
        char cmd[256];
        uint32_t n = 0;
        for (uint32_t i = 6; i + 1 < plen && n + 1 < sizeof(cmd); i += 2) {
            int hi = hex_val(packet[i]), lo = hex_val(packet[i + 1]);
            if (hi < 0 || lo < 0) break;
            cmd[n++] = (char)((hi << 4) | lo);
        }
        cmd[n] = 0;
        char text[1024];
        int tn = ops->monitor(ops->ctx, cmd, text, sizeof(text));
        if (tn <= 0) return emit(out, outcap, "OK");
        for (int k = 0; k < tn; k++) put_hex_byte(&o, (uint8_t)text[k]);
        return done(&o);
    }

    /* Kill: no reply is expected. Detach: acknowledge. */
    if (eq_lit(packet, plen, "k")) return GDBSTUB_NO_REPLY;
    if (plen >= 1 && packet[0] == 'D') return emit(out, outcap, "OK");

    /* Anything else: empty response => gdb treats the packet as unsupported. */
    return 0;
}
