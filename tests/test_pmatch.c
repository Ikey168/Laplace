/* Host tests for finding preemption points by state (#245). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmatch.h"

static int g_fail, g_pass;
#define CHECK(c, msg) do { if (c) g_pass++; else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

/* A tiny machine: registers and four pages at 0x10000. */
#define NPAGES 4
#define BASE   0x10000ULL
typedef struct {
    uint64_t regs[PM_NREGS];
    uint8_t  mem[NPAGES][PM_PAGE] __attribute__((aligned(8)));
    bool     present[NPAGES];
    uint8_t  insn[4];
    uint32_t insn_len;
} machine_t;

static bool read_page(void* ctx, uint64_t va, uint8_t* page) {
    machine_t* m = (machine_t*)ctx;
    uint64_t i = (va - BASE) / PM_PAGE;
    if (va < BASE || i >= NPAGES || !m->present[i]) return false;
    memcpy(page, m->mem[i], PM_PAGE);
    return true;
}

static void* xalloc(uint64_t n) { return malloc(n); }

static uint8_t g_payload[1 << 16] __attribute__((aligned(8)));

/* Record the machine's state, the pages in `dirty` (bit i: page i). */
static pm_view_t record(const machine_t* m, uint32_t dirty) {
    pm_builder_t b;
    pm_builder_init(&b, xalloc, free);
    for (uint32_t i = 0; i < NPAGES; i++) {
        if (dirty & (1u << i)) pm_builder_add(&b, BASE + i * PM_PAGE, m->present[i] ? m->mem[i] : 0);
    }
    pm_record_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.regs, m->regs, sizeof(h.regs));
    uint32_t n = pm_encode(&h, &b, g_payload, sizeof(g_payload));
    pm_builder_free(&b);
    pm_view_t v;
    if (!n || !pm_decode(g_payload, n, &v)) {
        printf("FAIL: encode/decode\n");
        exit(1);
    }
    return v;
}

static uint32_t insn_of(void* ctx, uint8_t* buf) {
    machine_t* m = (machine_t*)ctx;
    memcpy(buf, m->insn, m->insn_len);
    return m->insn_len;
}

static pm_verdict_t check(const pm_view_t* v, machine_t* m, uint32_t* hint) {
    static uint8_t scratch[PM_PAGE];
    pm_state_t s;
    memset(&s, 0, sizeof(s));
    memcpy(s.regs, m->regs, sizeof(s.regs));
    s.insn = insn_of;
    s.ctx = m;
    pm_mem_t mem = { read_page, m };
    return pm_check(v, &s, &mem, scratch, hint);
}

static void reset(machine_t* m) {
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < NPAGES; i++) m->present[i] = true;
}

/* A spin loop whose every iteration is the same state: the first candidate
 * matches, and its state is the recorded one. */
static void test_repeated_states(void) {
    static machine_t m, rec;
    reset(&m);
    m.regs[PM_RIP] = 0x401000;
    m.regs[PM_RAX] = 7;
    memset(m.mem[1], 0x42, PM_PAGE);
    rec = m;                                    /* preempted at iteration 9 of the same state */
    pm_view_t v = record(&rec, 0x2);
    uint32_t hint = 0;
    CHECK(check(&v, &m, &hint) == PM_MATCH, "an identical earlier state matches at once");
    CHECK(memcmp(&m, &rec, sizeof(m)) == 0, "and it is the recorded state");
}

/* A loop where only memory changes (incq (%rdi); jmp): the registers match
 * at every iteration, and only the recorded iteration's memory does. */
static void test_memory_only(void) {
    static machine_t m;
    reset(&m);
    m.regs[PM_RIP] = 0x401000;
    m.regs[PM_RDI] = BASE + 2 * PM_PAGE + 8;
    memset(m.mem[0], 1, PM_PAGE);               /* written before: unchanged */
    machine_t rec = m;
    *(uint64_t*)&rec.mem[2][8] = 1000;          /* preempted after 1000 increments */
    pm_view_t v = record(&rec, 0x5);            /* pages 0 and 2 dirty */
    uint32_t hint = 0, matched_at = 0, checks = 0;
    for (uint64_t i = 0; i <= 2000; i++) {
        *(uint64_t*)&m.mem[2][8] = i;
        checks++;
        if (check(&v, &m, &hint) == PM_MATCH) {
            matched_at = (uint32_t)i;
            break;
        }
    }
    CHECK(matched_at == 1000, "only the recorded iteration matches");
    CHECK(hint == 1, "the hint moved to the page that differs");
    (void)checks;

    /* A page of the record not present yet (demand paging) never matches. */
    m.present[0] = false;
    *(uint64_t*)&m.mem[2][8] = 1000;
    CHECK(check(&v, &m, &hint) == PM_NO, "an absent page differs from a present one");
}

/* rep movsb interrupted after 37 of 100 bytes: the candidate at the start of
 * the instruction asks to step, each iteration is checked, and the 37th
 * matches. */
static void test_rep_movsb(void) {
    static machine_t m, rec;
    reset(&m);
    m.regs[PM_RIP] = 0x401000;
    m.insn[0] = 0xF3; m.insn[1] = 0xA4; m.insn_len = 2;
    m.regs[PM_RSI] = BASE;
    m.regs[PM_RDI] = BASE + PM_PAGE;
    m.regs[PM_RCX] = 100;
    for (int i = 0; i < 100; i++) m.mem[0][i] = (uint8_t)(i * 7 + 1);
    rec = m;
    for (int i = 0; i < 37; i++) {              /* the recording's 37 iterations */
        rec.mem[1][rec.regs[PM_RDI] - BASE - PM_PAGE] = rec.mem[0][rec.regs[PM_RSI] - BASE];
        rec.regs[PM_RSI]++;
        rec.regs[PM_RDI]++;
        rec.regs[PM_RCX]--;
    }
    pm_view_t v = record(&rec, 0x2);
    uint32_t hint = 0;
    pm_verdict_t r = check(&v, &m, &hint);
    CHECK(r == PM_STEP, "the start of the instruction asks to step");
    int steps = 0;
    while (r == PM_STEP && steps < 200) {
        m.mem[1][m.regs[PM_RDI] - BASE - PM_PAGE] = m.mem[0][m.regs[PM_RSI] - BASE];
        m.regs[PM_RSI]++;
        m.regs[PM_RDI]++;
        m.regs[PM_RCX]--;
        steps++;
        r = check(&v, &m, &hint);
    }
    CHECK(r == PM_MATCH && steps == 37, "the 37th iteration matches");
    CHECK(memcmp(m.mem, rec.mem, sizeof(m.mem)) == 0 && m.regs[PM_RCX] == 63, "with the recorded state");

    /* Past the point (rcx below the recorded count): no stepping. */
    m.regs[PM_RCX] = 10; m.regs[PM_RSI] = BASE + 90; m.regs[PM_RDI] = BASE + PM_PAGE + 90;
    CHECK(check(&v, &m, &hint) == PM_NO, "a later iteration cannot reach it");
    /* Another execution of the same instruction with other pointers. */
    m.regs[PM_RCX] = 100; m.regs[PM_RSI] = BASE + 8; m.regs[PM_RDI] = BASE + PM_PAGE;
    CHECK(check(&v, &m, &hint) == PM_NO, "pointers that cannot reach it");
    /* The same registers, not a string instruction: no stepping. */
    m.insn[0] = 0x90; m.insn_len = 1;
    m.regs[PM_RCX] = 100; m.regs[PM_RSI] = BASE; m.regs[PM_RDI] = BASE + PM_PAGE;
    CHECK(check(&v, &m, &hint) == PM_NO, "only string instructions are stepped");
}

static void test_decoder(void) {
    const uint8_t a[] = { 0xF3, 0xA4 }, b[] = { 0xF3, 0x48, 0xA5 }, c[] = { 0xF2, 0xAE },
                  d[] = { 0xA4 }, e[] = { 0xF3, 0xC3 }, f[] = { 0x66, 0xF3, 0xAB },
                  g[] = { 0xF3, 0x0F, 0xB8 };
    CHECK(pm_is_rep_string(a, 2), "rep movsb");
    CHECK(pm_is_rep_string(b, 3), "rep movsq");
    CHECK(pm_is_rep_string(c, 2), "repne scasb");
    CHECK(!pm_is_rep_string(d, 1), "movsb without rep");
    CHECK(!pm_is_rep_string(e, 2), "rep ret");
    CHECK(pm_is_rep_string(f, 3), "rep stosw");
    CHECK(!pm_is_rep_string(g, 3), "popcnt (f3 0f b8)");
}

static void test_format(void) {
    static machine_t m;
    reset(&m);
    pm_view_t v = record(&m, 0xB);
    CHECK(v.hdr.nruns == 2 && v.hdr.npages == 3, "pages 0,1,3 make two runs");
    CHECK(v.runs[0].va == BASE && v.runs[0].pages == 2 && v.runs[1].va == BASE + 3 * PM_PAGE,
          "runs keep their addresses");
    uint32_t n = (uint32_t)(sizeof(pm_record_t) + 2 * sizeof(pm_run_t) + 3 * 8);
    uint64_t d = pm_digest(g_payload, n);
    g_payload[sizeof(pm_record_t) + 5] ^= 1;
    CHECK(pm_digest(g_payload, n) != d, "the digest covers the payload");
    pm_view_t w;
    CHECK(!pm_decode(g_payload, n - 8, &w), "a short payload is rejected");
}

int main(void) {
    test_decoder();
    test_format();
    test_repeated_states();
    test_memory_only();
    test_rep_movsb();
    printf("pmatch: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
