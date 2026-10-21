/* Laplace core: x86-64 CPU primitives, segments, and the trap frame (#219).
 *
 * The kernel runs with interrupts disabled; only user mode (and the machine
 * coroutine it runs in) takes interrupts. Every trap, from user or kernel mode,
 * builds a trap_frame_t and calls trap_dispatch().
 */

#ifndef CORE_CPU_H
#define CORE_CPU_H

#include <stdint.h>
#include <stdbool.h>

/* GDT selectors. User selectors carry RPL 3. */
#define SEL_KCODE   0x08
#define SEL_KDATA   0x10
#define SEL_UDATA   (0x18 | 3)
#define SEL_UCODE   (0x20 | 3)
#define SEL_TSS     0x28

#define RFLAGS_IF   (1ULL << 9)
#define RFLAGS_TF   (1ULL << 8)
#define RFLAGS_RF   (1ULL << 16)

/* Saved register state, in the order isr.asm lays it out on the stack. */
typedef struct trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} trap_frame_t;

/* Trap vectors the core handles. */
#define VEC_DIVIDE       0
#define VEC_DEBUG        1
#define VEC_BREAKPOINT   3
#define VEC_INVALID_OP   6
#define VEC_GPF          13
#define VEC_PAGE_FAULT   14
#define VEC_IRQ_BASE     32
#define VEC_TIMER        32
#define VEC_SYSCALL      128
/* Not an IDT vector: the SYSCALL instruction's entry stub tags its frames
 * with it (Linux system calls, #238). */
#define VEC_LINUX_SYSCALL 129

/* A kernel entry that is a system call, in either ABI. */
static inline int vec_is_syscall(uint64_t v) { return v == VEC_SYSCALL || v == VEC_LINUX_SYSCALL; }

#define MSR_EFER         0xC0000080
#define MSR_STAR         0xC0000081
#define MSR_LSTAR        0xC0000082
#define MSR_SFMASK       0xC0000084
#define MSR_FS_BASE      0xC0000100
#define MSR_GS_BASE      0xC0000101

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void io_wait(void) { outb(0x80, 0); }

static inline uint64_t read_cr2(void) {
    uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v;
}
static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void write_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}
static inline void invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline __attribute__((noreturn)) void cpu_halt_forever(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

/* Debug registers (hardware breakpoints / watchpoints, #228). */
static inline void write_dr(int n, uint64_t v) {
    switch (n) {
    case 0: __asm__ volatile("mov %0, %%dr0" : : "r"(v)); break;
    case 1: __asm__ volatile("mov %0, %%dr1" : : "r"(v)); break;
    case 2: __asm__ volatile("mov %0, %%dr2" : : "r"(v)); break;
    case 3: __asm__ volatile("mov %0, %%dr3" : : "r"(v)); break;
    case 6: __asm__ volatile("mov %0, %%dr6" : : "r"(v)); break;
    case 7: __asm__ volatile("mov %0, %%dr7" : : "r"(v)); break;
    default: break;
    }
}
static inline uint64_t read_dr6(void) {
    uint64_t v; __asm__ volatile("mov %%dr6, %0" : "=r"(v)); return v;
}

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline uint64_t read_cr4(void) {
    uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v;
}
static inline void write_cr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}
static inline void cpu_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

#define CR4_TSD (1ULL << 2)   /* user-mode RDTSC/RDTSCP raise #GP (#241) */

/* GDT + TSS + IDT; sets TSS.rsp0 to the trap stack. Sets CR4.TSD and, where
 * the CPU supports it, CPUID faulting (#241). */
void cpu_init(void);

/* RDMSR/WRMSR that survive a #GP from an MSR the CPU does not have. */
bool cpu_rdmsr_safe(uint32_t msr, uint64_t* out);
bool cpu_wrmsr_safe(uint32_t msr, uint64_t value);
/* trap_dispatch: a kernel-mode #GP raised by a probe above. Skips the faulting
 * instruction and returns true; false when no probe is active. */
bool cpu_msr_probe_recover(trap_frame_t* frame);

/* ---- CPUID as user programs see it (#241) ----
 *
 * User-mode CPUID is nondeterministic across hosts and advertises instructions
 * the recorder cannot trap (RDRAND, RDSEED, RDPID, TSX). Where the CPU supports
 * CPUID faulting, user CPUID raises #GP and the kernel answers from this table:
 * the host's leaves with every feature the recorder cannot replay masked off.
 * The table is part of the keyframe's kernel state, so a recording replays
 * with the answers it was recorded with. Without faulting, user CPUID runs
 * natively and the table only documents what the host reports. */
#define CPU_CPUID_MAX 128
#define CPU_CPUID_INDEXED 1u      /* the entry is specific to its subleaf */

typedef struct {
    uint32_t leaf, sub, flags;
    uint32_t r[4];                /* eax, ebx, ecx, edx */
} cpuid_entry_t;

typedef struct {
    uint32_t count;
    uint32_t faulting;            /* 1: user CPUID is answered from the table */
    cpuid_entry_t e[CPU_CPUID_MAX];
} cpuid_table_t;

void cpu_cpuid_init(void);
bool cpu_cpuid_faulting(void);
const cpuid_table_t* cpu_cpuid_table(void);
/* Adopt a recorded table (keyframe restore). */
void cpu_cpuid_set_table(const cpuid_table_t* t);
/* Answer a user CPUID from the table: out = eax, ebx, ecx, edx. */
void cpu_cpuid_lookup(uint32_t leaf, uint32_t sub, uint32_t out[4]);
/* Top of the trap stack (TSS.rsp0): user-mode traps land here. */
uint64_t cpu_trap_stack_top(void);
/* True when the CPU has RDRAND (entropy source for kentropy). */
bool cpu_has_rdrand(void);
bool cpu_rdrand64(uint64_t* out);

/* isr.asm */
void trap_dispatch(trap_frame_t* frame);
void trap_return(trap_frame_t* frame) __attribute__((noreturn));
void machine_enter(trap_frame_t* frame);
void syscall_entry(void);
void machine_exit(void) __attribute__((noreturn));

/* 8259 PIC and 8253 PIT. */
void pic_init(void);
void pic_eoi(uint8_t irq);
void pit_init(uint32_t hz);

#endif /* CORE_CPU_H */
