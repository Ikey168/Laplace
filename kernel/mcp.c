/* IKOS Orthogonal Persistence - MCP Time-Travel Interface (#159 Milestone E, #229)
 *
 * See include/mcp.h. Pure: no allocator, no I/O. JSON is scanned in place
 * (enough of JSON for JSON-RPC requests: nested objects, strings with escapes,
 * numbers) and responses are built into the caller's buffer.
 */

#include "mcp.h"

/* ---------------- small string builder ---------------- */

typedef struct { char* buf; uint32_t cap; uint32_t len; bool ovf; } sb_t;

static void sb_init(sb_t* s, char* buf, uint32_t cap) {
    s->buf = buf; s->cap = cap; s->len = 0; s->ovf = false;
}
static void sb_putc(sb_t* s, char c) {
    if (s->len < s->cap) s->buf[s->len++] = c; else s->ovf = true;
}
static void sb_put(sb_t* s, const char* str) {
    while (*str) sb_putc(s, *str++);
}
static void sb_putn(sb_t* s, const char* str, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) sb_putc(s, str[i]);
}
static void sb_put_u64(sb_t* s, uint64_t v) {
    char tmp[20]; int n = 0;
    if (v == 0) { sb_putc(s, '0'); return; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) sb_putc(s, tmp[n]);
}
static void sb_put_hex(sb_t* s, uint64_t v) {
    char tmp[16]; int n = 0;
    sb_put(s, "0x");
    do { int d = (int)(v & 0xF); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v);
    while (n--) sb_putc(s, tmp[n]);
}
/* Text destined for a JSON string: escape what JSON requires. */
static void sb_put_esc(sb_t* s, const char* str) {
    for (; *str; str++) {
        char c = *str;
        if (c == '"' || c == '\\') { sb_putc(s, '\\'); sb_putc(s, c); }
        else if (c == '\n') sb_put(s, "\\n");
        else if ((unsigned char)c < 0x20) sb_putc(s, ' ');
        else sb_putc(s, c);
    }
}

/* ---------------- JSON scanning ---------------- */

static bool str_eq(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}
static uint32_t lit_len(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* Locate `"key"` anywhere in json; returns the index just past the closing
 * quote, or -1. (The original helper; the dispatcher uses obj_get.) */
static int find_key(const char* json, uint32_t len, const char* key) {
    uint32_t klen = lit_len(key);
    if (klen + 2 > len) return -1;
    for (uint32_t i = 0; i + klen + 2 <= len; i++) {
        if (json[i] == '"' && str_eq(json + i + 1, key, klen) && json[i + 1 + klen] == '"')
            return (int)(i + 1 + klen + 1);
    }
    return -1;
}

/* Skip spaces and a single ':'. */
static uint32_t skip_colon(const char* json, uint32_t len, uint32_t i) {
    while (i < len && (json[i] == ' ' || json[i] == ':' || json[i] == '\t')) i++;
    return i;
}

bool mcp_json_int(const char* json, uint32_t len, const char* key, uint64_t* out) {
    int k = find_key(json, len, key);
    if (k < 0) return false;
    uint32_t i = skip_colon(json, len, (uint32_t)k);
    if (i >= len || json[i] < '0' || json[i] > '9') return false;
    uint64_t v = 0;
    while (i < len && json[i] >= '0' && json[i] <= '9') { v = v * 10 + (uint64_t)(json[i] - '0'); i++; }
    if (out) *out = v;
    return true;
}

bool mcp_json_str(const char* json, uint32_t len, const char* key,
                  char* out, uint32_t outcap) {
    int k = find_key(json, len, key);
    if (k < 0) return false;
    uint32_t i = skip_colon(json, len, (uint32_t)k);
    if (i >= len || json[i] != '"') return false;
    i++;
    uint32_t o = 0;
    while (i < len && json[i] != '"') {
        if (json[i] == '\\' && i + 1 < len) i++;
        if (o + 1 < outcap) out[o++] = json[i];
        i++;
    }
    if (o < outcap) out[o] = 0;
    return true;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_number(const char* p, uint32_t n, uint64_t* out) {
    uint64_t v = 0;
    uint32_t i = 0;
    if (n >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        for (i = 2; i < n && hexv(p[i]) >= 0; i++) v = (v << 4) | (uint64_t)hexv(p[i]);
        if (i == 2) return false;
    } else {
        for (; i < n && p[i] >= '0' && p[i] <= '9'; i++) v = v * 10 + (uint64_t)(p[i] - '0');
        if (i == 0) return false;
    }
    *out = v;
    return true;
}

bool mcp_json_num(const char* json, uint32_t len, const char* key, uint64_t* out) {
    int k = find_key(json, len, key);
    if (k < 0) return false;
    uint32_t i = skip_colon(json, len, (uint32_t)k);
    if (i >= len) return false;
    if (json[i] == '"') {
        uint32_t j = i + 1;
        while (j < len && json[j] != '"') j++;
        return parse_number(json + i + 1, j - i - 1, out);
    }
    return parse_number(json + i, len - i, out);
}

typedef struct { const char* p; uint32_t n; } span_t;

/* End (exclusive) of the JSON value starting at i. */
static uint32_t value_end(const char* j, uint32_t len, uint32_t i) {
    if (i >= len) return len;
    if (j[i] == '"') {
        for (i++; i < len; i++) {
            if (j[i] == '\\') { i++; continue; }
            if (j[i] == '"') return i + 1;
        }
        return len;
    }
    if (j[i] == '{' || j[i] == '[') {
        int depth = 0;
        bool in_str = false;
        for (; i < len; i++) {
            char c = j[i];
            if (in_str) {
                if (c == '\\') i++;
                else if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') in_str = true;
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') { if (--depth == 0) return i + 1; }
        }
        return len;
    }
    while (i < len && j[i] != ',' && j[i] != '}' && j[i] != ']' && j[i] != ' ' && j[i] != '\n') i++;
    return i;
}

/* The value of `key` among the direct members of the object `obj`. */
static bool obj_get(span_t obj, const char* key, span_t* val) {
    const char* j = obj.p;
    uint32_t len = obj.n, klen = lit_len(key), i = 0;
    while (i < len && j[i] != '{') i++;
    if (i >= len) return false;
    i++;
    while (i < len) {
        while (i < len && (j[i] == ' ' || j[i] == ',' || j[i] == '\n' || j[i] == '\t' || j[i] == '\r')) i++;
        if (i >= len || j[i] == '}') return false;
        if (j[i] != '"') return false;
        uint32_t kend = value_end(j, len, i);
        bool match = kend - i == klen + 2 && str_eq(j + i + 1, key, klen);
        i = kend;
        while (i < len && (j[i] == ' ' || j[i] == ':' || j[i] == '\t')) i++;
        uint32_t vend = value_end(j, len, i);
        if (match) {
            val->p = j + i;
            val->n = vend - i;
            return true;
        }
        i = vend;
    }
    return false;
}

static bool span_str(span_t v, char* out, uint32_t cap) {
    if (v.n < 2 || v.p[0] != '"') return false;
    uint32_t o = 0;
    for (uint32_t i = 1; i + 1 < v.n && o + 1 < cap; i++) {
        if (v.p[i] == '\\' && i + 2 < v.n) i++;
        out[o++] = v.p[i];
    }
    out[o] = 0;
    return true;
}

static bool span_num(span_t v, uint64_t* out) {
    if (v.n >= 2 && v.p[0] == '"') return parse_number(v.p + 1, v.n - 2, out);
    return parse_number(v.p, v.n, out);
}

static bool arg_num(span_t args, const char* key, uint64_t* out) {
    span_t v;
    return args.n && obj_get(args, key, &v) && span_num(v, out);
}

static bool streq_lit(const char* a, const char* lit) {
    uint32_t n = lit_len(lit);
    return lit_len(a) == n && str_eq(a, lit, n);
}

/* ---------------- response helpers ---------------- */

/* The request id, echoed verbatim (a number or a string). */
typedef struct { span_t raw; bool present; } rid_t;

static void put_id(sb_t* s, const rid_t* id) {
    if (id->present) sb_putn(s, id->raw.p, id->raw.n);
    else sb_put(s, "null");
}

static void begin_text_result(sb_t* s, const rid_t* id) {
    sb_put(s, "{\"jsonrpc\":\"2.0\",\"id\":");
    put_id(s, id);
    sb_put(s, ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"");
}
static void end_text_result(sb_t* s, bool is_error) {
    sb_put(s, "\"}]");
    if (is_error) sb_put(s, ",\"isError\":true");
    sb_put(s, "}}");
}
static int finish(sb_t* s) { return s->ovf ? -1 : (int)s->len; }

static int text_result(sb_t* s, const rid_t* id, const char* text, bool is_error) {
    begin_text_result(s, id);
    sb_put_esc(s, text);
    end_text_result(s, is_error);
    return finish(s);
}

/* " insn=I pid=P rip=0x..." and the edge / stop notes. */
static void put_pos_tail(sb_t* s, const mcp_pos_t* p) {
    sb_put(s, " insn="); sb_put_u64(s, p->insn);
    sb_put(s, " pid="); sb_put_u64(s, p->pid);
    sb_put(s, " rip="); sb_put_hex(s, p->rip);
    if (p->stop) { sb_put(s, " stop="); sb_put_esc(s, p->stop); }
    if (p->edge == 1) sb_put(s, " (start of the recording)");
    if (p->edge == 2) sb_put(s, " (end of the recording)");
}

static void put_pos(sb_t* s, const mcp_pos_t* p) {
    sb_put(s, "epoch="); sb_put_u64(s, p->epoch);
    sb_put(s, " offset="); sb_put_u64(s, p->step);
    put_pos_tail(s, p);
}

static bool where(const mcp_ops_t* ops, mcp_pos_t* p) {
    p->stop = 0;
    p->edge = 0;
    return ops->where && ops->where(ops->ctx, p) == 0;
}

/* ---------------- tool schemas ---------------- */

#define NO_ARGS "{\"type\":\"object\",\"properties\":{}}"

typedef struct { const char* name; const char* description; const char* schema; } tool_t;

static const tool_t g_tools[] = {
    { "list_checkpoints",
      "Retained keyframe window (the rewind horizon): oldest and newest epoch.", NO_ARGS },
    { "where",
      "Current position (epoch, offset = kernel entries into the epoch, insn), the current process, "
      "its instruction pointer, and why the machine stopped.", NO_ARGS },
    { "rewind_to",
      "Reconstruct a past moment by restoring the nearest keyframe and re-executing: epoch, offset "
      "(kernel entries into it), optional insn (0 = at that entry, 1 = right after it was handled, "
      "k = after k-1 more instructions).",
      "{\"type\":\"object\",\"properties\":{\"epoch\":{\"type\":\"integer\"},\"offset\":{\"type\":\"integer\"},"
      "\"insn\":{\"type\":\"integer\"}},\"required\":[\"epoch\"]}" },
    { "reverse_step", "Step one kernel entry backward through recorded history.", NO_ARGS },
    { "step_forward", "Step one kernel entry forward through recorded history.", NO_ARGS },
    { "reverse_stepi", "Step one instruction backward.", NO_ARGS },
    { "stepi", "Step one instruction forward.", NO_ARGS },
    { "read_memory",
      "Read up to 256 bytes of a process's memory at the current position.",
      "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"integer\"},\"addr\":{\"type\":[\"integer\",\"string\"],"
      "\"description\":\"address, e.g. 1073746176 or \\\"0x40001100\\\"\"},\"len\":{\"type\":\"integer\"}},"
      "\"required\":[\"pid\",\"addr\",\"len\"]}" },
    { "get_registers", "A process's registers at the current position.",
      "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"integer\"}},\"required\":[\"pid\"]}" },
    { "list_processes", "Processes alive at the current position.", NO_ARGS },
    { "watch_last_write",
      "Find where len bytes at addr in process pid last changed, at or before the current position, "
      "and land there: the exact instruction after the write (or the system call that wrote them).",
      "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"integer\"},\"addr\":{\"type\":[\"integer\",\"string\"]},"
      "\"len\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8}}}" },
    { "verify_replay",
      "Re-execute every retained epoch from its keyframe and compare the reconstructed state "
      "(memory, registers, process table, scheduler) with the checksums recorded live.", NO_ARGS },
    { "resume",
      "Leave recorded history and run the machine live (recording) until it stops again.", NO_ARGS },
};

/* ---------------- dispatch ---------------- */

static int tool_call(const mcp_ops_t* ops, sb_t* s, const rid_t* id, const char* name, span_t args) {
    char msg[160];
    mcp_pos_t pos = { 0 };

    if (streq_lit(name, "list_checkpoints")) {
        uint64_t oldest = 0, newest = 0; uint32_t count = 0;
        int rc = ops && ops->list_checkpoints
                 ? ops->list_checkpoints(ops->ctx, &oldest, &newest, &count) : -1;
        if (rc != 0) return text_result(s, id, "no checkpoints", true);
        begin_text_result(s, id);
        sb_put(s, "checkpoints: count="); sb_put_u64(s, count);
        sb_put(s, " oldest="); sb_put_u64(s, oldest);
        sb_put(s, " newest="); sb_put_u64(s, newest);
        end_text_result(s, false);
        return finish(s);
    }

    if (streq_lit(name, "where")) {
        if (!ops || !where(ops, &pos)) return text_result(s, id, "position unavailable", true);
        begin_text_result(s, id);
        sb_put(s, "position ");
        put_pos(s, &pos);
        end_text_result(s, false);
        return finish(s);
    }

    if (streq_lit(name, "rewind_to")) {
        uint64_t epoch = 0, offset = 0, insn = 0;
        bool have = arg_num(args, "epoch", &epoch);
        arg_num(args, "offset", &offset);
        arg_num(args, "insn", &insn);
        int rc;
        if (insn && ops && ops->goto_insn) rc = ops->goto_insn(ops->ctx, epoch, offset, insn);
        else rc = (have && ops && ops->rewind_to) ? ops->rewind_to(ops->ctx, epoch, offset) : -1;
        begin_text_result(s, id);
        if (rc == 0) {
            sb_put(s, "rewound to epoch="); sb_put_u64(s, epoch);
            sb_put(s, " offset="); sb_put_u64(s, offset);
            if (where(ops, &pos)) put_pos_tail(s, &pos);
            end_text_result(s, false);
        } else {
            sb_put(s, "rewind failed (outside the retained window?)");
            end_text_result(s, true);
        }
        return finish(s);
    }

    if (streq_lit(name, "reverse_step")) {
        uint64_t e = 0, o = 0;
        int rc = ops && ops->reverse_step ? ops->reverse_step(ops->ctx, &e, &o) : -1;
        begin_text_result(s, id);
        if (rc == 0) {
            sb_put(s, "stepped back to epoch="); sb_put_u64(s, e);
            sb_put(s, " offset="); sb_put_u64(s, o);
            if (where(ops, &pos)) put_pos_tail(s, &pos);
            end_text_result(s, false);
        } else {
            sb_put(s, "at the start of the retained window");
            end_text_result(s, true);
        }
        return finish(s);
    }

    if (streq_lit(name, "step_forward") || streq_lit(name, "reverse_stepi") || streq_lit(name, "stepi")) {
        if (!ops || !ops->move) return text_result(s, id, "unsupported", true);
        bool backward = streq_lit(name, "reverse_stepi");
        bool stepi = !streq_lit(name, "step_forward");
        int rc = ops->move(ops->ctx, backward, stepi);
        if (rc < 0) return text_result(s, id, "step failed", true);
        begin_text_result(s, id);
        sb_put(s, rc == 1 ? "no further history; at " : "now at ");
        if (where(ops, &pos)) put_pos(s, &pos);
        end_text_result(s, false);
        return finish(s);
    }

    if (streq_lit(name, "read_memory")) {
        uint64_t pid = 0, addr = 0, len = 0;
        if (!ops || !ops->read_memory || !arg_num(args, "pid", &pid) || !arg_num(args, "addr", &addr) ||
            !arg_num(args, "len", &len) || len == 0 || len > MCP_MAX_READ) {
            return text_result(s, id, "read_memory needs pid, addr, and len (1..256)", true);
        }
        uint8_t buf[MCP_MAX_READ];
        if (ops->read_memory(ops->ctx, (uint32_t)pid, addr, buf, (uint32_t)len) != 0) {
            return text_result(s, id, "address not mapped in that process", true);
        }
        begin_text_result(s, id);
        sb_put(s, "pid="); sb_put_u64(s, pid);
        sb_put(s, " addr="); sb_put_hex(s, addr);
        sb_put(s, " len="); sb_put_u64(s, len);
        sb_put(s, " bytes=");
        for (uint32_t i = 0; i < (uint32_t)len; i++) {
            const char* d = "0123456789abcdef";
            sb_putc(s, d[buf[i] >> 4]);
            sb_putc(s, d[buf[i] & 0xF]);
        }
        if (len == 4 || len == 8) {
            uint64_t v = 0;
            for (uint32_t i = 0; i < (uint32_t)len; i++) v |= (uint64_t)buf[i] << (8 * i);
            sb_put(s, len == 4 ? " u32=" : " u64=");
            sb_put_u64(s, v);
        }
        end_text_result(s, false);
        return finish(s);
    }

    if (streq_lit(name, "get_registers")) {
        uint64_t pid = 0;
        mcp_regs_t r;
        if (!ops || !ops->get_registers || !arg_num(args, "pid", &pid) ||
            ops->get_registers(ops->ctx, (uint32_t)pid, &r) != 0) {
            return text_result(s, id, "no such process at this position", true);
        }
        static const char* names[] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                       "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
                                       "rip", "rflags" };
        const uint64_t* v = &r.rax;
        begin_text_result(s, id);
        sb_put(s, "pid="); sb_put_u64(s, pid);
        for (int i = 0; i < 18; i++) {
            sb_putc(s, ' ');
            sb_put(s, names[i]);
            sb_putc(s, '=');
            sb_put_hex(s, v[i]);
        }
        end_text_result(s, false);
        return finish(s);
    }

    if (streq_lit(name, "list_processes")) {
        if (!ops || !ops->list_processes) return text_result(s, id, "unsupported", true);
        mcp_proc_t procs[16];
        uint32_t n = ops->list_processes(ops->ctx, procs, 16);
        begin_text_result(s, id);
        for (uint32_t i = 0; i < n; i++) {
            if (i) sb_put(s, "\\n");
            sb_put(s, "pid="); sb_put_u64(s, procs[i].pid);
            sb_put(s, " name="); sb_put_esc(s, procs[i].name);
            if (procs[i].current) sb_put(s, " current");
        }
        if (n == 0) sb_put(s, "no processes");
        end_text_result(s, false);
        return finish(s);
    }

    if (streq_lit(name, "watch_last_write")) {
        uint64_t pid = 0, addr = 0, len = 4;
        bool targeted = arg_num(args, "pid", &pid) && arg_num(args, "addr", &addr);
        arg_num(args, "len", &len);
        if (targeted && ops && ops->watch_memory) {
            int rc = ops->watch_memory(ops->ctx, (uint32_t)pid, addr, (uint32_t)len);
            if (rc < 0) return text_result(s, id, "watch failed", true);
            begin_text_result(s, id);
            if (rc == 1) {
                sb_put(s, "unchanged within the retained window; now at ");
            } else {
                sb_put(s, "last write at ");
            }
            if (where(ops, &pos)) put_pos(s, &pos);
            end_text_result(s, false);
            return finish(s);
        }
        uint64_t e = 0, o = 0;
        int rc = ops && ops->watch_last_write ? ops->watch_last_write(ops->ctx, &e, &o) : -1;
        begin_text_result(s, id);
        if (rc == 0) {
            sb_put(s, "last write at epoch="); sb_put_u64(s, e);
            sb_put(s, " offset="); sb_put_u64(s, o);
            end_text_result(s, false);
        } else {
            sb_put(s, "no write found in the retained window");
            end_text_result(s, true);
        }
        return finish(s);
    }

    if (streq_lit(name, "verify_replay")) {
        mcp_verify_t v = { 0 };
        if (!ops || !ops->verify || ops->verify(ops->ctx, &v) != 0) {
            return text_result(s, id, "verification could not run", true);
        }
        begin_text_result(s, id);
        sb_put(s, v.epochs_diverged ? "DIVERGED: " : "byte-exact: ");
        sb_put(s, "epochs_checked="); sb_put_u64(s, v.epochs_checked);
        sb_put(s, " epochs_diverged="); sb_put_u64(s, v.epochs_diverged);
        if (v.epochs_diverged) {
            sb_put(s, " first_epoch="); sb_put_u64(s, v.first_epoch);
            sb_put(s, " component="); sb_put_u64(s, v.first_component);
        }
        end_text_result(s, v.epochs_diverged != 0);
        return finish(s);
    }

    if (streq_lit(name, "resume")) {
        if (!ops || !ops->resume || ops->resume(ops->ctx) != 0) {
            return text_result(s, id, "resume failed", true);
        }
        begin_text_result(s, id);
        sb_put(s, "ran live and stopped at ");
        if (where(ops, &pos)) put_pos(s, &pos);
        end_text_result(s, false);
        return finish(s);
    }

    (void)msg;
    return text_result(s, id, "unknown tool", true);
}

int mcp_handle(const mcp_ops_t* ops, const char* request, uint32_t reqlen,
               char* out, uint32_t outcap) {
    span_t req = { request, reqlen };
    rid_t id = { { 0, 0 }, false };
    id.present = obj_get(req, "id", &id.raw);

    char method[48];
    span_t mv;
    if (!obj_get(req, "method", &mv) || !span_str(mv, method, sizeof(method))) {
        /* Not a request (a response, or garbage): nothing to answer unless it
         * carried an id. */
        if (!id.present) return 0;
        sb_t s; sb_init(&s, out, outcap);
        sb_put(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
        put_id(&s, &id);
        sb_put(&s, ",\"error\":{\"code\":-32600,\"message\":\"invalid request\"}}");
        return finish(&s);
    }

    /* Notifications (no id) never get a response. */
    if (!id.present) return 0;

    sb_t s; sb_init(&s, out, outcap);
    span_t params = { 0, 0 };
    obj_get(req, "params", &params);

    if (streq_lit(method, "initialize")) {
        char version[32] = "2024-11-05";
        span_t pv;
        if (params.n && obj_get(params, "protocolVersion", &pv)) span_str(pv, version, sizeof(version));
        sb_put(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
        put_id(&s, &id);
        sb_put(&s, ",\"result\":{\"protocolVersion\":\"");
        sb_put_esc(&s, version);
        sb_put(&s, "\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"laplace\",\"version\":\"1.0\"},"
                   "\"instructions\":\"Laplace records a whole machine and can reconstruct any past moment. "
                   "Positions are (epoch, offset, insn). Start with where and list_processes, use "
                   "watch_last_write to find who wrote a value, and read_memory / get_registers to inspect "
                   "the reconstructed state.\"}}");
        return finish(&s);
    }

    if (streq_lit(method, "ping")) {
        sb_put(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
        put_id(&s, &id);
        sb_put(&s, ",\"result\":{}}");
        return finish(&s);
    }

    if (streq_lit(method, "tools/list")) {
        sb_put(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
        put_id(&s, &id);
        sb_put(&s, ",\"result\":{\"tools\":[");
        for (uint32_t i = 0; i < sizeof(g_tools) / sizeof(g_tools[0]); i++) {
            if (i) sb_putc(&s, ',');
            sb_put(&s, "{\"name\":\"");
            sb_put(&s, g_tools[i].name);
            sb_put(&s, "\",\"description\":\"");
            sb_put_esc(&s, g_tools[i].description);
            sb_put(&s, "\",\"inputSchema\":");
            sb_put(&s, g_tools[i].schema);
            sb_putc(&s, '}');
        }
        sb_put(&s, "]}}");
        return finish(&s);
    }

    if (streq_lit(method, "tools/call")) {
        char name[48];
        span_t nv, args = { 0, 0 };
        if (!params.n || !obj_get(params, "name", &nv) || !span_str(nv, name, sizeof(name))) {
            return text_result(&s, &id, "missing tool name", true);
        }
        obj_get(params, "arguments", &args);
        return tool_call(ops, &s, &id, name, args);
    }

    /* Unknown method: JSON-RPC method-not-found error. */
    sb_put(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
    put_id(&s, &id);
    sb_put(&s, ",\"error\":{\"code\":-32601,\"message\":\"method not found\"}}");
    return finish(&s);
}
