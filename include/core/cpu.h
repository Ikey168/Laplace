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

/* GDT + TSS + IDT; sets TSS.rsp0 to the trap stack. */
void cpu_init(void);
/* Top of the trap stack (TSS.rsp0): user-mode traps land here. */
uint64_t cpu_trap_stack_top(void);
/* True when the CPU has RDRAND (entropy source for kentropy). */
bool cpu_has_rdrand(void);
bool cpu_rdrand64(uint64_t* out);

/* isr.asm */
void trap_dispatch(trap_frame_t* frame);
void trap_return(trap_frame_t* frame) __attribute__((noreturn));
void machine_enter(trap_frame_t* frame);
void machine_exit(void) __attribute__((noreturn));

/* 8259 PIC and 8253 PIT. */
void pic_init(void);
void pic_eoi(uint8_t irq);
void pit_init(uint32_t hz);

#endif /* CORE_CPU_H */
