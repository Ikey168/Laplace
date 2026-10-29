/* Laplace core: preemption at any user instruction, without a counter (#245).
 * See include/core/preempt.h and include/pmatch.h. */

#include "core/preempt.h"
#include "core/kio.h"
#include "core/machine.h"
#include "core/mm.h"
#include "core/proc.h"
#include "core/fpu.h"
#include "core/console.h"
#include "iorec.h"
#include "crc32_ieee.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
int   memcmp(const void* a, const void* b, size_t n);

static pm_builder_t    g_b;
static uint8_t*        g_buf;            /* a record's payload (8-byte aligned) */
static uint32_t        g_cap;
static bool            g_armed;
static uint32_t        g_pid;
static uint64_t        g_step;
static pm_view_t       g_view;
static uint32_t        g_hint;
static bool            g_stepped;        /* the armed point needed string-instruction steps */
static preempt_stats_t g_stats;
static uint8_t         g_scratch[PM_PAGE] __attribute__((aligned(8)));
/* Replay: the process's whole state at the last power-of-two check, to
 * notice a loop that cannot reach the point (see preempt_cycled). */
static struct {
    uint64_t checks;
    bool     have, have_mem, cycled;
    uint64_t regs[PM_NREGS], fs, gs;
    uint32_t fpu, mem;
} g_cyc;

static void* alloc(uint64_t n) { return kmalloc(n); }
static void release(void* p) { kfree(p); }

void preempt_init(void) { pm_builder_init(&g_b, alloc, release); }
const preempt_stats_t* preempt_stats(void) { return &g_stats; }

static void reserve(uint32_t n) {
    if (n <= g_cap) return;
    uint32_t cap = g_cap ? g_cap : 4096;
    while (cap < n) cap *= 2;
    if (g_buf) kfree(g_buf);
    g_buf = (uint8_t*)kmalloc(cap);
    if (!g_buf) panic("preempt: out of memory for a %u-byte record", n);
    g_cap = cap;
}

static void ctx_regs(const process_context_t* c, uint64_t* r) {
    r[PM_RAX] = c->rax; r[PM_RBX] = c->rbx; r[PM_RCX] = c->rcx; r[PM_RDX] = c->rdx;
    r[PM_RSI] = c->rsi; r[PM_RDI] = c->rdi; r[PM_RBP] = c->rbp; r[PM_RSP] = c->rsp;
    r[PM_R8] = c->r8;   r[PM_R9] = c->r9;   r[PM_R10] = c->r10; r[PM_R11] = c->r11;
    r[PM_R12] = c->r12; r[PM_R13] = c->r13; r[PM_R14] = c->r14; r[PM_R15] = c->r15;
    r[PM_RIP] = c->rip; r[PM_RFLAGS] = c->rflags;
}

/* The instruction bytes at rip (fewer at the end of the mapped code). */
static uint32_t insn_at(process_t* p, uint64_t rip, uint8_t* b) {
    if (uaccess_read(p->address_space, rip, b, 15)) return 15;
    uint32_t n = (uint32_t)(PM_PAGE - (rip & (PM_PAGE - 1)));
    if (n > 15) n = 15;
    return uaccess_read(p->address_space, rip, b, n) ? n : 0;
}

static uint32_t fpu_crc_of(process_t* p) {
    machine_fpu_flush();
    return fpu_checksum(0, proc_arch(p)->fpu);
}

/* A candidate: the process and its rip. */
typedef struct {
    process_t* p;
    uint64_t   rip;
} cand_t;

static uint32_t fpu_crc(void* ctx) { return fpu_crc_of(((cand_t*)ctx)->p); }

static uint32_t insn_cb(void* ctx, uint8_t* buf) {
    cand_t* c = (cand_t*)ctx;
    return insn_at(c->p, c->rip, buf);
}

/* ---- Recording ---- */

typedef struct {
    bool ok;
    bool current;      /* the space is loaded: flush the cleared entries */
} walk_t;

/* A page the CPU marked dirty, or one mapped on demand: hash it, and clear
 * the marks for the next record. */
static void dirty_page(void* ctx, uint64_t va, pte_t* pte) {
    walk_t* w = (walk_t*)ctx;
    if (!(*pte & PAGE_USER) || !(*pte & (PAGE_DIRTY | PAGE_PM_NEW))) return;
    if (!pm_builder_add(&g_b, va, (const uint8_t*)P2V(*pte & LP_PTE_ADDR_MASK))) w->ok = false;
    *pte &= ~(pte_t)(PAGE_DIRTY | PAGE_PM_NEW);
    if (w->current) vmm_flush_tlb_page(va);
}

void preempt_record(process_t* p, uint64_t step) {
    pm_builder_reset(&g_b);
    walk_t w = { true, vmm_get_current_space() == p->address_space };
    vmm_for_each_pte(p->address_space, dirty_page, &w);
    if (!w.ok) panic("preempt: out of memory hashing pages");

    pm_record_t h;
    memset(&h, 0, sizeof(h));
    h.pid = (uint32_t)p->pid;
    ctx_regs(&p->context, h.regs);
    h.fs_base = proc_arch(p)->fs_base;
    h.gs_base = proc_arch(p)->gs_base;
    h.fpu_crc = fpu_crc_of(p);
    uint8_t insn[15];
    h.rep = pm_is_rep_string(insn, insn_at(p, p->context.rip, insn)) ? 1 : 0;

    uint32_t n = pm_encoded_size(&g_b);
    reserve(n);
    pm_encode(&h, &g_b, g_buf, g_cap);
    kio_record(IOREC_PREEMPT, step, pm_digest(g_buf, n), g_buf, n);
    g_stats.recorded++;
    g_stats.recorded_rep += h.rep;
    g_stats.record_pages += g_b.npages;
}

/* ---- Replay ---- */

void preempt_disarm(void) { g_armed = false; }

bool preempt_armed(uint64_t* step) {
    if (g_armed && step) *step = g_step;
    return g_armed;
}

bool preempt_armed_for(const process_t* p, uint64_t* rip) {
    if (!g_armed || !p || (uint32_t)p->pid != g_pid) return false;
    *rip = g_view.hdr.regs[PM_RIP];
    return true;
}

preempt_arm_t preempt_arm(process_t* p, uint64_t step) {
    g_armed = false;
    uint64_t args;
    uint32_t len = 0;
    while (!kio_record_next(IOREC_PREEMPT, step, &args, g_buf, g_cap, &len)) {
        if (len <= g_cap) return PREEMPT_NONE;
        reserve(len);
    }
    if (pm_digest(g_buf, len) != args || !pm_decode(g_buf, len, &g_view) || !p ||
        g_view.hdr.pid != (uint32_t)p->pid) {
        kio_note_diverged(IOREC_PREEMPT, step);
        return PREEMPT_BAD;
    }
    g_armed = true;
    g_pid = g_view.hdr.pid;
    g_step = step;
    memset(&g_cyc, 0, sizeof(g_cyc));
    g_hint = 0;
    g_stepped = false;
    return PREEMPT_ARMED;
}

uint32_t preempt_rep_len(process_t* p, uint64_t rip) {
    uint8_t insn[15];
    pm_rep_t r;
    return pm_rep_decode(insn, insn_at(p, rip, insn), &r) ? r.len : 0;
}

bool preempt_rep_forward(process_t* p, process_context_t* c) {
    uint8_t insn[15];
    pm_rep_t r;
    if (!g_armed || !pm_rep_decode(insn, insn_at(p, c->rip, insn), &r) || r.addr32) return false;
    if ((r.op != PM_REP_MOVS && r.op != PM_REP_STOS) || (c->rflags & 0x400)) return false;
    uint64_t want = g_view.hdr.regs[PM_RCX];
    if (c->rcx < want) return false;
    uint64_t n = (c->rcx - want) * r.size;
    while (n) {
        uint64_t chunk = n < PM_PAGE ? n : PM_PAGE;
        if (r.op == PM_REP_MOVS) {
            /* Overlapping forward copies read what earlier elements wrote:
             * never copy further than the gap at once. */
            uint64_t gap = c->rdi > c->rsi ? c->rdi - c->rsi : 0;
            if (gap && gap < chunk) chunk = gap;
            chunk -= chunk % r.size;
            if (!chunk) chunk = r.size;
            if (!uaccess_read(p->address_space, c->rsi, g_scratch, chunk)) return false;
        } else {
            for (uint64_t i = 0; i < chunk; i++) g_scratch[i] = (uint8_t)(c->rax >> (8 * (i % r.size)));
        }
        if (!uaccess_write(p->address_space, c->rdi, g_scratch, chunk)) return false;
        c->rdi += chunk;
        if (r.op == PM_REP_MOVS) c->rsi += chunk;
        c->rcx -= chunk / r.size;
        n -= chunk;
    }
    g_stats.forwarded++;
    return true;
}

/* A page as the record saw it: present or not (uaccess_read would give an
 * untouched page as zeros, but the record tells the two apart). */
static bool read_page(void* ctx, uint64_t va, uint8_t* page) {
    pte_t* pte = vmm_get_page_table(((process_t*)ctx)->address_space, va, PT_LEVEL, false);
    if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_USER)) return false;
    memcpy(page, P2V(*pte & LP_PTE_ADDR_MASK), PM_PAGE);
    return true;
}

static void mem_page(void* ctx, uint64_t va, uint64_t phys, pte_t pte) {
    (void)pte;
    uint32_t* crc = (uint32_t*)ctx;
    *crc = crc32_ieee(*crc, &va, sizeof(va));
    *crc = crc32_ieee(*crc, P2V(phys), PM_PAGE);
}

/* A candidate that is not the point. Every 2^k-th, its state is compared
 * with the previous 2^k-th: registers first, the user memory only when they
 * are the same. The same state twice means the process, which runs alone
 * and without an entry until its point, repeats those checks forever and
 * never reaches a state it did not already show (a keyframe restored
 * wrongly, say, under a loop that waits for a signal). */
static void note_miss(process_t* p, const pm_state_t* s) {
    uint64_t n = ++g_cyc.checks;
    if (n & (n - 1)) return;
    uint32_t fpu = fpu_crc_of(p);
    bool same = g_cyc.have && g_cyc.fs == s->fs_base && g_cyc.gs == s->gs_base && g_cyc.fpu == fpu &&
                memcmp(g_cyc.regs, s->regs, sizeof(g_cyc.regs)) == 0;
    uint32_t mem = 0;
    if (same) vmm_for_each_page(p->address_space, mem_page, &mem);
    if (same && g_cyc.have_mem && g_cyc.mem == mem) g_cyc.cycled = true;
    memcpy(g_cyc.regs, s->regs, sizeof(g_cyc.regs));
    g_cyc.fs = s->fs_base;
    g_cyc.gs = s->gs_base;
    g_cyc.fpu = fpu;
    g_cyc.mem = mem;
    g_cyc.have = true;
    g_cyc.have_mem = same;
}

bool preempt_cycled(void) {
    if (!g_armed || !g_cyc.cycled) return false;
    kio_note_diverged(IOREC_PREEMPT, g_step);
    return true;
}

pm_verdict_t preempt_check(process_t* p, const process_context_t* c) {
    if (!g_armed || (uint32_t)p->pid != g_pid || c->rip != g_view.hdr.regs[PM_RIP]) return PM_NO;
    g_stats.candidates++;
    pm_state_t s;
    memset(&s, 0, sizeof(s));
    ctx_regs(c, s.regs);
    s.fs_base = proc_arch(p)->fs_base;
    s.gs_base = proc_arch(p)->gs_base;
    cand_t cand = { p, c->rip };
    s.insn = insn_cb;
    s.fpu_crc = fpu_crc;
    s.ctx = &cand;
    pm_mem_t mem = { read_page, p };
    pm_verdict_t v = pm_check(&g_view, &s, &mem, g_scratch, &g_hint);
    if (v == PM_STEP) g_stepped = true;
    if (v == PM_NO) note_miss(p, &s);
    if (v == PM_MATCH) {
        g_stats.matched++;
        if (g_stepped) g_stats.matched_rep++;
    }
    return v;
}
