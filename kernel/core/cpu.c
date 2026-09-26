/* Laplace core: GDT, TSS, IDT, 8259 PIC, and 8253 PIT (#219, #220). */

#include "core/cpu.h"
#include "core/console.h"

/* ---- GDT + TSS ---- */

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} desc_ptr_t;

static tss_t g_tss;
static uint64_t g_gdt[7];

/* The trap stack: TSS.rsp0, where every user-mode trap lands. */
static uint8_t g_trap_stack[65536] __attribute__((aligned(16)));

uint64_t cpu_trap_stack_top(void) {
    return (uint64_t)(g_trap_stack + sizeof(g_trap_stack));
}

static void gdt_init(void) {
    g_gdt[0] = 0;
    g_gdt[1] = 0x00209A0000000000ULL;  /* 0x08 kernel code (L=1) */
    g_gdt[2] = 0x0000920000000000ULL;  /* 0x10 kernel data */
    g_gdt[3] = 0x0000F20000000000ULL;  /* 0x18 user data (DPL 3) */
    g_gdt[4] = 0x0020FA0000000000ULL;  /* 0x20 user code (DPL 3, L=1) */

    g_tss.rsp0 = cpu_trap_stack_top();
    g_tss.iomap_base = sizeof(tss_t);   /* no I/O bitmap: user port I/O faults */

    uint64_t base = (uint64_t)&g_tss;
    uint64_t limit = sizeof(tss_t) - 1;
    g_gdt[5] = (limit & 0xFFFF)
             | ((base & 0xFFFFFF) << 16)
             | (0x89ULL << 40)                   /* present, 64-bit TSS (available) */
             | (((limit >> 16) & 0xF) << 48)
             | (((base >> 24) & 0xFF) << 56);
    g_gdt[6] = base >> 32;

    desc_ptr_t gp = { sizeof(g_gdt) - 1, (uint64_t)g_gdt };
    __asm__ volatile("lgdt %0" : : "m"(gp));
    /* Reload CS through a far return, then the data segments and TR. */
    __asm__ volatile(
        "pushq %0\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %1, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        : : "i"((uint64_t)SEL_KCODE), "i"((uint16_t)SEL_KDATA) : "rax", "memory");
    __asm__ volatile("ltr %0" : : "r"((uint16_t)SEL_TSS));
}

/* ---- IDT ---- */

typedef struct __attribute__((packed)) {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} idt_entry_t;

static idt_entry_t g_idt[256];
extern const uint64_t isr_stub_table[49];

static void idt_set(int vec, uint64_t handler, uint8_t dpl) {
    idt_entry_t* e = &g_idt[vec];
    e->off_lo = handler & 0xFFFF;
    e->sel = SEL_KCODE;
    e->ist = 0;
    e->type_attr = (uint8_t)(0x8E | (dpl << 5));   /* present, interrupt gate */
    e->off_mid = (handler >> 16) & 0xFFFF;
    e->off_hi = (uint32_t)(handler >> 32);
    e->zero = 0;
}

static void idt_init(void) {
    for (int v = 0; v < 48; v++) idt_set(v, isr_stub_table[v], 0);
    /* int3 and int 0x80 are reachable from user mode. */
    idt_set(VEC_BREAKPOINT, isr_stub_table[VEC_BREAKPOINT], 3);
    idt_set(VEC_SYSCALL, isr_stub_table[48], 3);
    desc_ptr_t ip = { sizeof(g_idt) - 1, (uint64_t)g_idt };
    __asm__ volatile("lidt %0" : : "m"(ip));
}

void cpu_init(void) {
    gdt_init();
    idt_init();
}

/* ---- 8259 PIC ---- */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void pic_init(void) {
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, VEC_IRQ_BASE); io_wait();       /* master: 32-39 */
    outb(PIC2_DATA, VEC_IRQ_BASE + 8); io_wait();   /* slave: 40-47 */
    outb(PIC1_DATA, 4); io_wait();
    outb(PIC2_DATA, 2); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    /* Only the timer (IRQ0) is unmasked; disk and serial I/O are polled. */
    outb(PIC1_DATA, 0xFE);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* ---- 8253 PIT ---- */

void pit_init(uint32_t hz) {
    uint32_t divisor = 1193182u / hz;
    outb(0x43, 0x36);                       /* channel 0, lo/hi, mode 3 */
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)(divisor >> 8));
}

/* ---- RDRAND ---- */

bool cpu_has_rdrand(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return (c >> 30) & 1;
}

bool cpu_rdrand64(uint64_t* out) {
    for (int i = 0; i < 10; i++) {
        uint64_t v;
        uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return true; }
    }
    return false;
}
