/* Laplace core: GDT, TSS, IDT, 8259 PIC, and 8253 PIT (#219, #220). */

#include "core/cpu.h"
#include "core/console.h"
#include "core/fpu.h"

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
extern uint64_t syscall_kernel_rsp;

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
    /* No IST: QEMU's TCG can deliver a second #DB at the handler's first
     * instruction, which an IST stack would overwrite the first frame with.
     * No #DB can arrive on the user's stack at the SYSCALL stub: SFMASK clears
     * TF on entry, and the stub touches no user memory. */
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
    /* User RDTSC/RDTSCP fault and are emulated from the journaled clock. */
    write_cr4(read_cr4() | CR4_TSD);
    fpu_init();

    /* SYSCALL (#238): kernel CS 0x08 / SS 0x10 on entry; SYSRET would use
     * 0x23 / 0x1B (the stub returns with iretq). Entry masks IF, TF, DF, AC,
     * and NT. EFER.SCE was set by boot.asm. */
    syscall_kernel_rsp = cpu_trap_stack_top();
    wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)SEL_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200 | 0x100 | 0x400 | 0x40000 | 0x4000);
}

/* ---- MSRs that may not exist ---- */

static volatile bool g_msr_probing;
static volatile bool g_msr_faulted;

bool cpu_msr_probe_recover(trap_frame_t* f) {
    if (!g_msr_probing || f->vector != VEC_GPF) return false;
    g_msr_faulted = true;
    f->rip += 2;                      /* RDMSR and WRMSR are both 0F 3x */
    return true;
}

bool cpu_rdmsr_safe(uint32_t msr, uint64_t* out) {
    uint32_t lo = 0, hi = 0;
    g_msr_faulted = false;
    g_msr_probing = true;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
    g_msr_probing = false;
    if (g_msr_faulted) return false;
    *out = ((uint64_t)hi << 32) | lo;
    return true;
}

bool cpu_wrmsr_safe(uint32_t msr, uint64_t value) {
    g_msr_faulted = false;
    g_msr_probing = true;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
    g_msr_probing = false;
    return !g_msr_faulted;
}

/* ---- CPUID as user programs see it (#241) ---- */

#define MSR_PLATFORM_INFO          0xCE
#define MSR_MISC_FEATURES_ENABLES  0x140
#define PLATFORM_INFO_CPUID_FAULT  (1ULL << 31)

static cpuid_table_t g_cpuid;

static bool leaf_indexed(uint32_t leaf) {
    switch (leaf) {
    case 0x4: case 0x7: case 0xB: case 0xD: case 0x1F: case 0x8000001D:
        return true;
    default:
        return false;
    }
}

/* What the recorder can replay. Everything not listed reads as 0. */
static void mask_leaf(cpuid_entry_t* e) {
    uint32_t* r = e->r;
    switch (e->leaf) {
    case 0x0:
        if (r[0] > 0xD) r[0] = 0xD;
        break;
    case 0x1:
        /* One CPU, APIC id 0. */
        r[1] = (r[1] & 0x0000FFFFu) | (1u << 16);
        /* SSE3 PCLMUL SSSE3 CX16 SSE4.1 SSE4.2 MOVBE POPCNT AES, and the
         * hypervisor bit. No RDRAND (untrappable), no XSAVE/AVX/FMA/F16C until
         * their state is saved (#237). */
        r[2] &= (1u << 0) | (1u << 1) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 20) |
                (1u << 22) | (1u << 23) | (1u << 25) | (1u << 31) |
                (fpu_has_xsave() ? (1u << 26) | (1u << 27) : 0) |              /* XSAVE OSXSAVE */
                ((fpu_xcr0() & XCR0_AVX) ? (1u << 12) | (1u << 28) | (1u << 29) : 0);  /* FMA AVX F16C */
        /* No PSN, DS, ACPI, SS, TM, PBE. */
        r[3] &= ~((1u << 18) | (1u << 21) | (1u << 22) | (1u << 27) | (1u << 29) | (1u << 31));
        break;
    case 0x4:
        r[0] &= ~0xFC000000u;          /* one core per package */
        break;
    case 0x7:
        if (e->sub != 0) { r[0] = r[1] = r[2] = r[3] = 0; break; }
        r[0] = 0;                      /* no further subleaves */
        /* BMI1 BMI2 ERMS ADX CLFLUSHOPT SHA. No RDSEED (untrappable), no TSX
         * (aborts are nondeterministic), no AVX2/AVX-512 (#237). */
        r[1] &= (1u << 3) | (1u << 8) | (1u << 9) | (1u << 19) | (1u << 23) | (1u << 29) |
                ((fpu_xcr0() & XCR0_AVX) ? (1u << 5) : 0);                      /* AVX2 */
        r[2] = 0;                      /* no RDPID, WAITPKG, PKU */
        r[3] &= (1u << 4);             /* FSRM */
        break;
    case 0xB:
    case 0x1F:
        r[3] = 0;                      /* x2APIC id 0 */
        break;
    case 0xD:
        /* XSAVE as the kernel enabled it: no XSAVEOPT/XSAVEC/XGETBV1/XSAVES
         * (subleaf 1), whose optimizations expose the processor's in-use
         * tracking. */
        if (!fpu_has_xsave() || (e->sub != 0 && e->sub != 2) ||
            (e->sub == 2 && !(fpu_xcr0() & XCR0_AVX))) {
            r[0] = r[1] = r[2] = r[3] = 0;
        } else if (e->sub == 0) {
            r[0] = (uint32_t)fpu_xcr0();
            r[1] = r[2] = fpu_area_size();
            r[3] = 0;
        }
        break;
    case 0x80000000:
        if (r[0] > 0x80000008) r[0] = 0x80000008;
        break;
    case 0x80000001:
        r[2] &= (1u << 0) | (1u << 5) | (1u << 6) | (1u << 8);   /* LAHF ABM SSE4A PREFETCHW */
        r[3] &= (1u << 11) | (1u << 20) | (1u << 26) | (1u << 27) | (1u << 29);
        break;
    case 0x80000002: case 0x80000003: case 0x80000004:   /* brand string */
    case 0x80000006: case 0x80000007: case 0x80000008:
        break;
    case 0x2: case 0x16:
        break;
    default:
        r[0] = r[1] = r[2] = r[3] = 0;
        break;
    }
}

static void add_leaf(uint32_t leaf, uint32_t sub, bool indexed) {
    if (g_cpuid.count >= CPU_CPUID_MAX) return;
    cpuid_entry_t* e = &g_cpuid.e[g_cpuid.count++];
    e->leaf = leaf;
    e->sub = sub;
    e->flags = indexed ? CPU_CPUID_INDEXED : 0;
    cpu_cpuid(leaf, sub, e->r);
    mask_leaf(e);
}

static void add_range(uint32_t first, uint32_t last) {
    for (uint32_t leaf = first; leaf <= last; leaf++) {
        if (!leaf_indexed(leaf)) {
            add_leaf(leaf, 0, false);
            continue;
        }
        for (uint32_t sub = 0; sub < 8; sub++) {
            uint32_t r[4];
            cpu_cpuid(leaf, sub, r);
            if (sub > 0 && leaf == 0x4 && (r[0] & 0x1F) == 0) break;
            if (sub > 0 && (leaf == 0xB || leaf == 0x1F) && ((r[2] >> 8) & 0xFF) == 0) break;
            if (sub > 0 && leaf == 0x7) break;
            add_leaf(leaf, sub, true);
        }
    }
}

void cpu_cpuid_init(void) {
    uint32_t r[4];
    g_cpuid.count = 0;
    cpu_cpuid(0, 0, r);
    add_range(0, r[0] < 0xD ? r[0] : 0xD);
    cpu_cpuid(0x80000000, 0, r);
    if (r[0] >= 0x80000000) add_range(0x80000000, r[0] < 0x80000008 ? r[0] : 0x80000008);

    uint64_t info = 0;
    g_cpuid.faulting = cpu_rdmsr_safe(MSR_PLATFORM_INFO, &info) &&
                       (info & PLATFORM_INFO_CPUID_FAULT) &&
                       cpu_wrmsr_safe(MSR_MISC_FEATURES_ENABLES, 1);
}

bool cpu_cpuid_faulting(void) { return g_cpuid.faulting != 0; }
const cpuid_table_t* cpu_cpuid_table(void) { return &g_cpuid; }

void cpu_cpuid_set_table(const cpuid_table_t* t) {
    uint32_t faulting = g_cpuid.faulting;   /* a property of this CPU, not the recording */
    g_cpuid = *t;
    if (g_cpuid.count > CPU_CPUID_MAX) g_cpuid.count = CPU_CPUID_MAX;
    g_cpuid.faulting = faulting;
}

void cpu_cpuid_lookup(uint32_t leaf, uint32_t sub, uint32_t out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    for (uint32_t i = 0; i < g_cpuid.count; i++) {
        const cpuid_entry_t* e = &g_cpuid.e[i];
        if (e->leaf != leaf) continue;
        if ((e->flags & CPU_CPUID_INDEXED) && e->sub != sub) continue;
        for (int k = 0; k < 4; k++) out[k] = e->r[k];
        return;
    }
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
