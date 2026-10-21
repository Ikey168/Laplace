/* Laplace core: user processes (#220). See include/core/proc.h. */

#include "core/proc.h"
#include "core/mm.h"
#include "core/vm.h"
#include "core/files.h"
#include "core/console.h"
#include "process_manager.h"
#include "core/machine.h"
#include "entropy_record.h"
#include "snapshot_store.h"   /* snapshot_crc32 */

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
int   strcmp(const char* a, const char* b);

static process_t* g_table[LP_MAX_PROCS];
static uint32_t   g_count;
static uint32_t   g_next_pid = 1;

/* ---- Embedded images ---- */

extern const uint8_t user_image_heisenbug[], user_image_heisenbug_end[];
extern const uint8_t user_image_noise[], user_image_noise_end[];
extern const uint8_t user_image_counter[], user_image_counter_end[];
extern const uint8_t user_image_hello[], user_image_hello_end[];
extern const uint8_t user_image_tscprobe[], user_image_tscprobe_end[];
extern const uint8_t user_image_rdrandleak[], user_image_rdrandleak_end[];
extern const uint8_t user_image_fpuprobe[], user_image_fpuprobe_end[];
extern const uint8_t user_image_musl_hello[], user_image_musl_hello_end[];
extern const uint8_t user_image_musl_sysprobe[], user_image_musl_sysprobe_end[];
extern const uint8_t user_image_musl_vmprobe[], user_image_musl_vmprobe_end[];
extern const uint8_t user_image_musl_bigmem[], user_image_musl_bigmem_end[];
extern const uint8_t user_image_musl_fileprobe[], user_image_musl_fileprobe_end[];
extern const uint8_t user_image_musl_inputprobe[], user_image_musl_inputprobe_end[];

static const lp_image_t g_images[] = {
    { "heisenbug", user_image_heisenbug, user_image_heisenbug_end, false },
    { "noise",     user_image_noise,     user_image_noise_end, false },
    { "counter",   user_image_counter,   user_image_counter_end, false },
    { "hello",     user_image_hello,     user_image_hello_end, false },
    { "tscprobe",  user_image_tscprobe,  user_image_tscprobe_end, false },
    { "rdrandleak", user_image_rdrandleak, user_image_rdrandleak_end, false },
    { "fpuprobe",  user_image_fpuprobe,  user_image_fpuprobe_end, false },
    { "musl_hello", user_image_musl_hello, user_image_musl_hello_end, true },
    { "musl_sysprobe", user_image_musl_sysprobe, user_image_musl_sysprobe_end, true },
    { "musl_vmprobe", user_image_musl_vmprobe, user_image_musl_vmprobe_end, true },
    { "musl_bigmem", user_image_musl_bigmem, user_image_musl_bigmem_end, true },
    { "musl_fileprobe", user_image_musl_fileprobe, user_image_musl_fileprobe_end, true },
    { "musl_inputprobe", user_image_musl_inputprobe, user_image_musl_inputprobe_end, true },
};

const lp_image_t* proc_images(uint32_t* count) {
    *count = sizeof(g_images) / sizeof(g_images[0]);
    return g_images;
}

/* ---- Programs loaded as Multiboot modules (#240) ---- */

#define MAX_MODULES 32
static lp_image_t g_modules[MAX_MODULES];
static char       g_module_paths[MAX_MODULES][64];
static uint32_t   g_nmodules;

void proc_register_modules(const uint32_t* mbi) {
    if (!(mbi[0] & (1u << 3))) return;
    uint32_t count = mbi[5];
    for (uint32_t i = 0; i < count && g_nmodules < MAX_MODULES; i++) {
        const uint32_t* m = (const uint32_t*)P2V(mbi[6] + 16ULL * i);
        /* The module's string is "path [args]": its first word names it. */
        const char* str = m[2] ? (const char*)P2V(m[2]) : "";
        char* path = g_module_paths[g_nmodules];
        uint32_t n = 0;
        while (str[n] && str[n] != ' ' && n < sizeof(g_module_paths[0]) - 1) {
            path[n] = str[n];
            n++;
        }
        path[n] = 0;
        lp_image_t* img = &g_modules[g_nmodules++];
        img->name = path;
        img->start = (const uint8_t*)P2V(m[0]);
        img->end = (const uint8_t*)P2V(m[1]);
        img->linux_abi = true;
        kprintf("boot: module %s (%lu bytes)\n", path, (uint64_t)m[1] - m[0]);
    }
}

static const char* basename_of(const char* path) {
    const char* b = path;
    for (const char* q = path; *q; q++) {
        if (*q == '/') b = q + 1;
    }
    return b;
}

/* An embedded program by name, or a module by path or by the last component
 * of its path (run=/musl_args.elf finds build/user/musl_args.elf). */
const lp_image_t* proc_find_image(const char* name) {
    for (uint32_t i = 0; i < sizeof(g_images) / sizeof(g_images[0]); i++) {
        if (strcmp(g_images[i].name, name) == 0) return &g_images[i];
    }
    for (uint32_t i = 0; i < g_nmodules; i++) {
        if (strcmp(g_modules[i].name, name) == 0) return &g_modules[i];
    }
    for (uint32_t i = 0; i < g_nmodules; i++) {
        if (strcmp(basename_of(g_modules[i].name), basename_of(name)) == 0) return &g_modules[i];
    }
    /* A program in the machine's filesystem (#247). */
    return files_find_image(name);
}

const lp_image_t* proc_seed_module(const char* root) {
    for (uint32_t i = 0; root && root[0] && i < g_nmodules; i++) {
        if (strcmp(g_modules[i].name, root) == 0 ||
            strcmp(basename_of(g_modules[i].name), basename_of(root)) == 0) {
            return &g_modules[i];
        }
    }
    /* Else the first cpio archive ("newc": "070701"). */
    for (uint32_t i = 0; i < g_nmodules; i++) {
        const uint8_t* b = g_modules[i].start;
        if (g_modules[i].end - b >= 6 && b[0] == '0' && b[1] == '7' && b[2] == '0' && b[3] == '7' &&
            b[4] == '0' && b[5] == '1') {
            return &g_modules[i];
        }
    }
    return 0;
}

uint32_t proc_image_crc(const lp_image_t* img) {
    return snapshot_crc32(0, img->start, (uint32_t)(img->end - img->start));
}

/* ---- Table ---- */

uint32_t proc_count(void) { return g_count; }
process_t* proc_at(uint32_t i) { return i < g_count ? g_table[i] : 0; }
uint32_t proc_next_pid(void) { return g_next_pid; }
void proc_set_next_pid(uint32_t pid) { g_next_pid = pid; }

process_t* proc_by_pid(uint32_t pid) {
    for (uint32_t i = 0; i < g_count; i++) {
        if ((uint32_t)g_table[i]->pid == pid) return g_table[i];
    }
    return 0;
}

int proc_index(const process_t* p) {
    for (uint32_t i = 0; i < g_count; i++) {
        if (g_table[i] == p) return (int)i;
    }
    return -1;
}

process_t* proc_next_ready(const process_t* p) {
    if (g_count == 0) return 0;
    int start = p ? proc_index(p) : -1;
    for (uint32_t k = 1; k <= g_count; k++) {
        process_t* c = g_table[(uint32_t)(start + (int)k) % g_count];
        if (c->state == PROCESS_STATE_READY || c->state == PROCESS_STATE_RUNNING) return c;
    }
    return 0;
}

void proc_append(process_t* p) {
    if (g_count >= LP_MAX_PROCS) panic("process table full");
    g_table[g_count++] = p;
}

static bool arch_alloc(process_t* p) {
    p->arch_block = kzalloc(sizeof(lp_arch_t) + 64);
    if (!p->arch_block) return false;
    p->arch = (void*)(((uint64_t)p->arch_block + 63) & ~63ULL);
    fpu_area_init(proc_arch(p)->fpu);
    return true;
}

uint32_t proc_context_record(process_t* p, uint8_t* page) {
    memset(page, 0, PAGE_SIZE);
    memcpy(page, &p->context, sizeof(p->context));
    memcpy(page + LP_CTX_ARCH_OFFSET, p->arch, sizeof(lp_arch_t));
    return LP_CTX_ARCH_OFFSET + sizeof(lp_arch_t);
}

void proc_context_load(process_t* p, const uint8_t* page) {
    memcpy(&p->context, page, sizeof(p->context));
    memcpy(p->arch, page + LP_CTX_ARCH_OFFSET, sizeof(lp_arch_t));
    fpu_sanitize(proc_arch(p)->fpu);
}

process_t* proc_alloc(uint32_t pid, const char* name) {
    process_t* p = (process_t*)kzalloc(sizeof(process_t));
    if (!p) return 0;
    if (!arch_alloc(p)) {
        kfree(p);
        return 0;
    }
    p->pid = (pid_t)pid;
    for (int i = 0; name && name[i] && i < MAX_PROCESS_NAME - 1; i++) p->name[i] = name[i];
    p->state = PROCESS_STATE_NEW;
    p->address_space = vmm_create_address_space(pid);
    if (!p->address_space) {
        kfree(p->arch_block);
        kfree(p);
        return 0;
    }
    return p;
}

void proc_destroy(process_t* p) {
    files_proc_gone(p);
    int idx = proc_index(p);
    if (idx >= 0) {
        for (uint32_t i = (uint32_t)idx; i + 1 < g_count; i++) g_table[i] = g_table[i + 1];
        g_count--;
    }
    machine_fpu_forget(p);
    vmm_destroy_address_space(p->address_space);
    kfree(p->arch_block);
    kfree(p);
}

void proc_destroy_all(void) {
    while (g_count) proc_destroy(g_table[g_count - 1]);
}

/* ---- ELF loading ---- */

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf64_phdr_t;

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2

static bool map_zeroed(vm_space_t* s, uint64_t va, uint32_t pte_flags) {
    uint64_t phys = pmm_alloc();
    if (!phys) return false;
    return vmm_map_page(s, va, phys, pte_flags) == 0;
}

/* ---- The System V initial stack (#238) ---- */

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_HWCAP2   26
#define AT_EXECFN   31

typedef struct {
    vm_space_t* s;
    uint64_t    sp;
    bool        ok;
} ustack_t;

static uint64_t push_bytes(ustack_t* u, const void* data, uint64_t len) {
    u->sp -= len;
    if (!uaccess_write(u->s, u->sp, data, len)) u->ok = false;
    return u->sp;
}

static void push_u64(ustack_t* u, uint64_t v) { push_bytes(u, &v, 8); }

/* argc = 1 (the program's name), no environment, and the auxiliary vector a
 * static Linux binary's startup code reads. No AT_SYSINFO_EHDR: without a
 * vDSO, libc reads the time through a (journaled) system call. AT_RANDOM's
 * bytes come from the entropy source; they are in the first keyframe. */
typedef struct {
    uint32_t           argc;
    const char* const* argv;
    uint32_t           envc;
    const char* const* envp;
} proc_args_t;

static uint64_t push_str(ustack_t* u, const char* str) {
    uint64_t n = 0;
    while (str[n]) n++;
    return push_bytes(u, str, n + 1);
}

/* argv, envp, and the auxiliary vector a static Linux binary's startup code
 * reads. No AT_SYSINFO_EHDR: without a vDSO, libc reads the time through a
 * (journaled) system call. AT_RANDOM's bytes come from the entropy source;
 * they are in the first keyframe. `base` is the load bias (static-PIE). */
#define MAX_ARGS 64
static bool linux_initial_stack(process_t* p, const elf64_ehdr_t* eh, uint64_t phdr_va,
                                uint64_t base, uint64_t top, const proc_args_t* a) {
    ustack_t u = { p->address_space, top, true };
    uint32_t argc = a->argc < MAX_ARGS ? a->argc : MAX_ARGS;
    uint32_t envc = a->envc < MAX_ARGS ? a->envc : MAX_ARGS;
    uint64_t argp[MAX_ARGS], envq[MAX_ARGS];
    for (uint32_t i = 0; i < envc; i++) envq[i] = push_str(&u, a->envp[i]);
    for (uint32_t i = 0; i < argc; i++) argp[i] = push_str(&u, a->argv[i]);
    uint64_t exec_ptr = argc ? argp[0] : push_str(&u, p->name);
    uint64_t plat_ptr = push_str(&u, "x86_64");
    uint8_t rnd[16];
    kentropy_fill(rnd, sizeof(rnd));
    uint64_t rand_ptr = push_bytes(&u, rnd, sizeof(rnd));
    u.sp &= ~15ULL;

    uint32_t r[4];
    cpu_cpuid(1, 0, r);
    const uint64_t aux[][2] = {
        { AT_PHDR, phdr_va }, { AT_PHENT, eh->phentsize }, { AT_PHNUM, eh->phnum },
        { AT_PAGESZ, PAGE_SIZE }, { AT_BASE, 0 }, { AT_FLAGS, 0 }, { AT_ENTRY, base + eh->entry },
        { AT_UID, 0 }, { AT_EUID, 0 }, { AT_GID, 0 }, { AT_EGID, 0 },
        { AT_PLATFORM, plat_ptr }, { AT_HWCAP, r[3] }, { AT_CLKTCK, 100 }, { AT_SECURE, 0 },
        { AT_RANDOM, rand_ptr }, { AT_HWCAP2, 0 }, { AT_EXECFN, exec_ptr }, { AT_NULL, 0 },
    };
    uint64_t naux = sizeof(aux) / sizeof(aux[0]);
    /* argc, argv..., NULL, envp..., NULL, auxv: rsp 16-byte aligned at argc. */
    uint64_t words = 1 + (argc + 1) + (envc + 1) + 2 * naux;
    if (words & 1) push_u64(&u, 0);
    for (uint64_t i = naux; i-- > 0;) {
        push_u64(&u, aux[i][1]);
        push_u64(&u, aux[i][0]);
    }
    push_u64(&u, 0);
    for (uint32_t i = envc; i-- > 0;) push_u64(&u, envq[i]);
    push_u64(&u, 0);
    for (uint32_t i = argc; i-- > 0;) push_u64(&u, argp[i]);
    push_u64(&u, argc);
    p->context.rsp = u.sp;
    return u.ok;
}

#define PT_INTERP    3
#define ET_DYN       3
#define PIE_BASE     0x555555554000ULL   /* where Linux puts a PIE (no ASLR) */

static bool load_elf(process_t* p, const uint8_t* img, uint64_t size, bool linux_abi,
                     const proc_args_t* args) {
    const elf64_ehdr_t* eh = (const elf64_ehdr_t*)img;
    if (size < sizeof(*eh) || eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' || eh->ident[4] != 2 ||
        eh->machine != 62 /* EM_X86_64 */) {
        return false;
    }
    vm_space_t* s = p->address_space;
    uint64_t phdr_va = 0;
    uint64_t brk = 0;             /* the page after the highest segment */
    /* A static PIE is loaded at a fixed base; a program that needs a dynamic
     * loader is not supported yet (#254). */
    uint64_t base = eh->type == ET_DYN ? PIE_BASE : 0;
    for (uint16_t i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t* ph = (const elf64_phdr_t*)(img + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type == PT_INTERP) {
            kprintf("load: %s is dynamically linked; not supported yet\n", p->name);
            return false;
        }
    }
    for (uint16_t i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t* ph = (const elf64_phdr_t*)(img + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (eh->phoff >= ph->offset && eh->phoff < ph->offset + ph->filesz) {
            phdr_va = base + ph->vaddr + (eh->phoff - ph->offset);
        }
        uint64_t vaddr = base + ph->vaddr;
        if (vaddr < LP_USER_BASE || vaddr + ph->memsz > LP_USER_END ||
            ph->offset + ph->filesz > size) {
            return false;
        }
        uint64_t start = vaddr & ~0xFFFULL;
        uint64_t end = PAGE_ALIGN(vaddr + ph->memsz);
        if (end > brk) brk = end;
        bool writable = (ph->flags & PF_W) != 0;
        uint32_t rflags = VMM_FLAG_READ | VMM_FLAG_USER |
                          (writable ? VMM_FLAG_WRITE : 0) | ((ph->flags & PF_X) ? VMM_FLAG_EXEC : 0);
        if (!vmm_create_region(s, start, end - start, rflags,
                               writable ? VMM_REGION_DATA : VMM_REGION_CODE,
                               writable ? "data" : "text")) {
            return false;
        }
        uint32_t pte_flags = PAGE_USER | (writable ? PAGE_WRITABLE : 0);
        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            if (!vmm_get_physical_addr(s, va) && !map_zeroed(s, va, pte_flags)) return false;
        }
        /* Copy the file bytes through the physical mapping. */
        for (uint64_t off = 0; off < ph->filesz; ) {
            uint64_t va = vaddr + off;
            uint64_t chunk = PAGE_SIZE - (va & 0xFFF);
            if (chunk > ph->filesz - off) chunk = ph->filesz - off;
            memcpy(P2V(vmm_get_physical_addr(s, va)), img + ph->offset + off, chunk);
            off += chunk;
        }
    }
    /* Stack. A Linux program's is 8 MiB at the top of the lower half, its
     * pages mapped as they are touched (#239); an embedded program's is a few
     * pages, all mapped. */
    uint64_t stack_top = linux_abi ? LP_LINUX_STACK_TOP : LP_STACK_TOP;
    uint64_t stack_lo = stack_top - (linux_abi ? LP_LINUX_STACK_SIZE : LP_STACK_PAGES * PAGE_SIZE);
    if (!vmm_create_region(s, stack_lo, stack_top - stack_lo,
                           VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_USER,
                           VMM_REGION_STACK, "stack")) {
        return false;
    }
    for (uint64_t va = stack_lo; !linux_abi && va < stack_top; va += PAGE_SIZE) {
        if (!map_zeroed(s, va, PAGE_USER | PAGE_WRITABLE)) return false;
    }
    if (linux_abi) vm_set_brk(p, brk);

    memset(&p->context, 0, sizeof(p->context));
    p->context.rip = base + eh->entry;
    p->context.rsp = stack_top - 8;        /* as if _start had been called */
    p->context.rflags = RFLAGS_IF | 0x2;
    p->context.cs = SEL_UCODE;
    p->context.ss = SEL_UDATA;
    p->context.ds = p->context.es = SEL_UDATA;
    p->entry_point = base + eh->entry;
    p->stack_start = stack_lo;
    p->stack_end = stack_top;
    if (linux_abi) return linux_initial_stack(p, eh, phdr_va, base, stack_top, args);
    return true;
}

process_t* proc_spawn_args(const lp_image_t* image, uint32_t argc, const char* const* argv,
                           uint32_t envc, const char* const* envp) {
    process_t* p = proc_alloc(g_next_pid, basename_of(image->name));
    if (!p) return 0;
    proc_args_t args = { argc, argv, envc, envp };
    if (!load_elf(p, image->start, (uint64_t)(image->end - image->start), image->linux_abi,
                  &args)) {
        vmm_destroy_address_space(p->address_space);
        kfree(p->arch_block);
        kfree(p);
        return 0;
    }
    /* What it runs (#240): the debuggers need the binary, and a recording
     * resumed with a different one is refused. */
    for (uint32_t i = 0; image->name[i] && i < sizeof(p->exe_path) - 1; i++) {
        p->exe_path[i] = image->name[i];
    }
    p->exe_crc = proc_image_crc(image);
    files_proc_new(p);
    g_next_pid++;
    p->state = PROCESS_STATE_READY;
    proc_append(p);
    return p;
}

process_t* proc_spawn(const lp_image_t* image) {
    const char* argv[1] = { image->name };
    return proc_spawn_args(image, 1, argv, 0, 0);
}

/* ---- Context <-> frame ---- */

void ctx_from_frame(process_context_t* c, const trap_frame_t* f) {
    c->rax = f->rax; c->rbx = f->rbx; c->rcx = f->rcx; c->rdx = f->rdx;
    c->rsi = f->rsi; c->rdi = f->rdi; c->rbp = f->rbp; c->rsp = f->rsp;
    c->r8 = f->r8;   c->r9 = f->r9;   c->r10 = f->r10; c->r11 = f->r11;
    c->r12 = f->r12; c->r13 = f->r13; c->r14 = f->r14; c->r15 = f->r15;
    c->rip = f->rip;
    c->rflags = f->rflags;
    c->cs = (uint16_t)f->cs;
    c->ss = (uint16_t)f->ss;
}

void ctx_to_frame(const process_context_t* c, trap_frame_t* f) {
    f->rax = c->rax; f->rbx = c->rbx; f->rcx = c->rcx; f->rdx = c->rdx;
    f->rsi = c->rsi; f->rdi = c->rdi; f->rbp = c->rbp; f->rsp = c->rsp;
    f->r8 = c->r8;   f->r9 = c->r9;   f->r10 = c->r10; f->r11 = c->r11;
    f->r12 = c->r12; f->r13 = c->r13; f->r14 = c->r14; f->r15 = c->r15;
    f->rip = c->rip;
    /* User mode always runs with interrupts on and IOPL 0. */
    f->rflags = (c->rflags & ~0x3000ULL) | RFLAGS_IF | 0x2;
    f->cs = SEL_UCODE;
    f->ss = SEL_UDATA;
    f->vector = 0;
    f->error = 0;
}

/* ---- The API the checkpoint engine and divergence sources call ---- */

process_t* pm_get_process(uint32_t pid) { return proc_by_pid(pid); }
process_t* process_get_by_pid(pid_t pid) { return proc_by_pid((uint32_t)pid); }

int pm_get_process_list(uint32_t* pids, uint32_t max_count, uint32_t* count_out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_count && n < max_count; i++) pids[n++] = (uint32_t)g_table[i]->pid;
    *count_out = n;
    return 0;
}

int pm_table_add_process(process_t* process) {
    if (!process || proc_index(process) >= 0) return -1;
    proc_append(process);
    return 0;
}

process_t* process_create(const char* name, const char* path) {
    (void)path;
    process_t* p = proc_alloc(g_next_pid++, name);
    if (p) p->state = PROCESS_STATE_READY;
    return p;
}

int scheduler_add_process(process_t* proc) {
    if (!proc) return -1;
    if (proc_index(proc) < 0) proc_append(proc);
    proc->state = PROCESS_STATE_READY;
    return 0;
}
