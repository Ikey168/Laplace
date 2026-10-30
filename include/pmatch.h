/* Laplace: finding an asynchronous preemption point again by its state (#245).
 *
 * While recording, a timer interrupt that lands in user mode can preempt the
 * process at the interrupted instruction. No counter says how far it got, so
 * the point is described by state instead:
 *
 *   - the general registers, rip, and the arithmetic flags;
 *   - the FS and GS bases and a checksum of the x87/SSE/AVX state;
 *   - a hash of every page the process may have written since its last kernel
 *     entry: the pages whose dirty bit the CPU set since the previous
 *     preemption, which cleared them. Pages outside that set are the same at
 *     every point of the segment.
 *
 * A replay arms a hardware breakpoint at the recorded rip for that process
 * and checks each hit: registers first, then the extended state, then the
 * pages. The first point that matches is the recorded one, or one whose whole
 * state equals it, so its future is the same. An interrupt inside a repeated
 * string instruction leaves rip on the instruction with rcx, rsi, rdi
 * partly advanced: the check then asks to single-step its iterations.
 *
 * A record is one payload in the input journal (#242):
 *
 *   pm_record_t | nruns x pm_run_t | npages x uint64_t page hash
 *
 * Pure: page contents come from a callback. Host-tested
 * (tests/test_pmatch.c).
 */

#ifndef PMATCH_H
#define PMATCH_H

#include <stdint.h>
#include <stdbool.h>

#define PM_VERSION 1

enum {
    PM_RAX, PM_RBX, PM_RCX, PM_RDX, PM_RSI, PM_RDI, PM_RBP, PM_RSP,
    PM_R8, PM_R9, PM_R10, PM_R11, PM_R12, PM_R13, PM_R14, PM_R15,
    PM_RIP, PM_RFLAGS,
    PM_NREGS
};
/* CF PF AF ZF SF DF OF: the flags a program can observe. */
#define PM_RFLAGS_MASK 0xED5ULL

#define PM_PAGE 4096u

typedef struct {
    uint32_t version;
    uint32_t pid;
    uint64_t regs[PM_NREGS];
    uint64_t fs_base, gs_base;
    uint32_t fpu_crc;
    uint32_t nruns;
    uint32_t npages;
    uint32_t rep;          /* recorded inside a repeated string instruction */
    uint64_t branches;     /* retired branches since the entry; 0: no counter */
} pm_record_t;

typedef struct {
    uint64_t va;
    uint64_t pages;
} pm_run_t;

/* A decoded record: views into the payload. */
typedef struct {
    pm_record_t     hdr;
    const pm_run_t* runs;
    const uint64_t* hashes;
} pm_view_t;

typedef struct {
    /* Copy the page at `va` into `page`; false when it is not present. */
    bool (*read_page)(void* ctx, uint64_t va, uint8_t* page);
    void* ctx;
} pm_mem_t;

/* The state at a candidate point. */
typedef struct {
    uint64_t regs[PM_NREGS];
    uint64_t fs_base, gs_base;
    /* The bytes at rip (up to 15; returns how many), read only when the
     * point may be inside a string instruction. */
    uint32_t (*insn)(void* ctx, uint8_t* buf);
    /* The extended-state checksum, computed only when the registers match. */
    uint32_t (*fpu_crc)(void* ctx);
    void* ctx;
} pm_state_t;

typedef enum {
    PM_NO = 0,     /* not this point */
    PM_MATCH,      /* the recorded point */
    PM_STEP        /* the recorded string instruction, earlier iteration: single-step it */
} pm_verdict_t;

uint64_t pm_page_hash(uint64_t va, const uint8_t* page /* 0: not present */);

/* Is `insn` a REP/REPE/REPNE string instruction (movs, stos, lods, cmps,
 * scas, ins, outs)? */
bool pm_is_rep_string(const uint8_t* insn, uint32_t len);

typedef enum {
    PM_REP_MOVS, PM_REP_STOS, PM_REP_LODS, PM_REP_CMPS, PM_REP_SCAS, PM_REP_INS, PM_REP_OUTS
} pm_rep_op_t;
typedef struct {
    pm_rep_op_t op;
    uint32_t    size;      /* element bytes */
    uint32_t    len;       /* instruction bytes */
    bool        addr32;    /* 0x67: ecx/esi/edi */
    bool        uses_si, uses_di;
} pm_rep_t;
bool pm_rep_decode(const uint8_t* insn, uint32_t len, pm_rep_t* out);

/* Recording: collect pages in ascending address order. */
typedef struct {
    pm_run_t* runs;
    uint64_t* hashes;
    uint32_t  nruns, rcap;
    uint32_t  npages, hcap;
    void*   (*mem_alloc)(uint64_t bytes);
    void    (*mem_free)(void* p);
} pm_builder_t;

void pm_builder_init(pm_builder_t* b, void* (*mem_alloc)(uint64_t), void (*mem_free)(void*));
bool pm_builder_add(pm_builder_t* b, uint64_t va, const uint8_t* page);
void pm_builder_reset(pm_builder_t* b);
void pm_builder_free(pm_builder_t* b);

/* The encoded size, and encoding into `out` (returns the bytes written, 0 if
 * `cap` is too small). hdr.nruns and hdr.npages are taken from `b`. */
uint32_t pm_encoded_size(const pm_builder_t* b);
uint32_t pm_encode(const pm_record_t* hdr, const pm_builder_t* b, uint8_t* out, uint32_t cap);
/* Decode a payload (8-byte aligned). False when it is malformed. */
bool     pm_decode(const uint8_t* buf, uint32_t len, pm_view_t* v);
/* A digest of a payload: the record's journal arguments. */
uint64_t pm_digest(const uint8_t* buf, uint32_t len);

/* Check a candidate point. `scratch` holds one page; `*hint` is the index of
 * the page to compare first (the last one that differed), updated. */
pm_verdict_t pm_check(const pm_view_t* v, const pm_state_t* s, const pm_mem_t* mem,
                      uint8_t* scratch, uint32_t* hint);

#endif /* PMATCH_H */
