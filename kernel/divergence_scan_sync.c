/* IKOS Orthogonal Persistence - Divergence component scan adapter (#197, #225)
 *
 * See include/divergence_scan.h. Wires the pure scan core to the global
 * divergence detector (#166), a registry of live component sources, and the
 * journal: on a record run it checksums each registered component and stashes
 * the sums for journaling; on replay it installs the recorded sums and compares
 * the recomputed ones, so a nondeterminism leak halts at the exact epoch and
 * component.
 *
 * The component sources cover what "byte-exact" means for the recorded machine
 * (#225): the process table, the scheduler (current process, run order, and the
 * pending kernel entry), the contents of every mapped user page, and every
 * process's saved registers. Physical addresses and page-table permission bits
 * are excluded: a restored machine maps the same contents at the same virtual
 * addresses through different frames, and copy-on-write marking changes
 * permissions without changing anything a program can observe.
 */

#include "divergence_scan.h"
#include "divergence.h"          /* kdiverge_*, divergence_checksum */
#include "process_manager.h"     /* pm_get_process_list, pm_get_process */
#include "core/proc.h"           /* process table order */
#include "core/mm.h"             /* vmm_for_each_page */
#include "core/machine.h"        /* machine_current, machine_pending */
#include <stddef.h>

/* ---- Source registry ---- */

static diverge_source_t g_sources[KDIVERGE_COMPONENT_COUNT];
static uint32_t         g_nsources;

/* Sums stashed by the last record scan, for journaling. */
static uint32_t g_rec_ids[KDIVERGE_COMPONENT_COUNT];
static uint32_t g_rec_sums[KDIVERGE_COMPONENT_COUNT];
static uint32_t g_rec_count;

void kdiverge_reset_sources(void) {
    g_nsources = 0;
}

void kdiverge_register(uint32_t component, diverge_source_fn source, void* ctx) {
    if (!source || g_nsources >= KDIVERGE_COMPONENT_COUNT) return;
    g_sources[g_nsources].component = component;
    g_sources[g_nsources].source = source;
    g_sources[g_nsources].ctx = ctx;
    g_nsources++;
}

/* ---- Concrete component sources over the recorded machine ---- */

/* Process table: pid, liveness, and name of every process, in table order. */
static uint32_t sum_proctable(void* ctx) {
    (void)ctx;
    uint32_t crc = 0;
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* p = proc_at(i);
        uint32_t pid = (uint32_t)p->pid;
        uint32_t alive = p->state != PROCESS_STATE_TERMINATED;
        crc = divergence_checksum(crc, &pid, sizeof(pid));
        crc = divergence_checksum(crc, &alive, sizeof(alive));
        crc = divergence_checksum(crc, p->name, sizeof(p->name));
    }
    return crc;
}

/* Scheduler: the current process, the run order, and the kernel entry the
 * current process is stopped at. */
static uint32_t sum_scheduler(void* ctx) {
    (void)ctx;
    process_t* cur = machine_current();
    uint32_t cpid = cur ? (uint32_t)cur->pid : 0xFFFFFFFFu;
    uint32_t crc = divergence_checksum(0, &cpid, sizeof(cpid));
    for (uint32_t i = 0; i < proc_count(); i++) {
        uint32_t pid = (uint32_t)proc_at(i)->pid;
        crc = divergence_checksum(crc, &pid, sizeof(pid));
    }
    uint64_t vec = machine_pending()->vector;
    return divergence_checksum(crc, &vec, sizeof(vec));
}

/* User memory: every mapped page's address and contents, per process. */
typedef struct { uint32_t crc; } page_sum_t;
static void sum_one_page(void* c, uint64_t virt, uint64_t phys, pte_t pte) {
    (void)pte;
    page_sum_t* s = (page_sum_t*)c;
    s->crc = divergence_checksum(s->crc, &virt, sizeof(virt));
    s->crc = divergence_checksum(s->crc, (const void*)phys, PAGE_SIZE);
}
static uint32_t sum_user_pages(void* ctx) {
    (void)ctx;
    page_sum_t s = { 0 };
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* p = proc_at(i);
        uint32_t pid = (uint32_t)p->pid;
        s.crc = divergence_checksum(s.crc, &pid, sizeof(pid));
        vmm_for_each_page(p->address_space, sum_one_page, &s);
    }
    return s.crc;
}

/* Registers: the general registers, instruction pointer, stack pointer, and
 * the program-visible flags (arithmetic, direction, interrupt) of every
 * process. Trap and resume flags belong to the debugger, not the program. */
static uint32_t sum_contexts(void* ctx) {
    (void)ctx;
    uint32_t crc = 0;
    for (uint32_t i = 0; i < proc_count(); i++) {
        const process_context_t* c = &proc_at(i)->context;
        uint64_t regs[19] = {
            c->rax, c->rbx, c->rcx, c->rdx, c->rsi, c->rdi, c->rbp, c->rsp,
            c->r8, c->r9, c->r10, c->r11, c->r12, c->r13, c->r14, c->r15,
            c->rip, c->rflags & 0xED5ULL, (uint64_t)proc_at(i)->pid,
        };
        crc = divergence_checksum(crc, regs, sizeof(regs));
    }
    return crc;
}

void kdiverge_register_kernel_sources(void) {
    kdiverge_reset_sources();
    kdiverge_register(KDIVERGE_PROCTABLE, sum_proctable, NULL);
    kdiverge_register(KDIVERGE_SCHEDULER, sum_scheduler, NULL);
    kdiverge_register(KDIVERGE_USER_PAGES, sum_user_pages, NULL);
    kdiverge_register(KDIVERGE_CONTEXTS, sum_contexts, NULL);
}

/* ---- Record side ---- */

void kdiverge_record_epoch(uint64_t epoch) {
    kdiverge_set_mode(DIVERGE_RECORD);
    kdiverge_begin_epoch(epoch);
    g_rec_count = diverge_scan_record(g_sources, g_nsources, kdiverge_record,
                                      g_rec_ids, g_rec_sums);
}

uint32_t kdiverge_journal_sums(const uint32_t** ids, const uint32_t** sums) {
    if (ids) *ids = g_rec_ids;
    if (sums) *sums = g_rec_sums;
    return g_rec_count;
}

/* ---- Replay side ---- */

int kdiverge_expect_pairs(uint64_t epoch, const uint32_t* ids,
                          const uint32_t* sums, uint32_t n) {
    /* Build a dense array indexed by component id: divergence_expect installs
     * sums[i] for component i. Components not in the journal stay 0; only
     * registered components are ever checked, so unused slots do not matter. */
    uint32_t dense[DIVERGE_MAX_COMPONENTS];
    for (uint32_t i = 0; i < DIVERGE_MAX_COMPONENTS; i++) dense[i] = 0;
    uint32_t span = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (ids[i] >= DIVERGE_MAX_COMPONENTS) continue;
        dense[ids[i]] = sums[i];
        if (ids[i] + 1 > span) span = ids[i] + 1;
    }
    return kdiverge_expect(epoch, dense, span);
}

bool kdiverge_check_epoch(void) {
    return diverge_scan_check(g_sources, g_nsources, kdiverge_check);
}
