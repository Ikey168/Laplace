/* Laplace core: the kernel state a keyframe holds (#243). See include/core/kstate.h. */

#include "core/kstate.h"
#include "core/proc.h"
#include "core/mm.h"
#include "core/cpu.h"
#include "core/clock.h"
#include "core/machine.h"
#include "core/console.h"
#include "divergence.h"
#include "divergence_scan.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

static ksf_registry_t g_reg;
static uint32_t       g_perturb_id;      /* 0: none */
static uint64_t       g_restored_now_ns;
static uint64_t       g_epoch;           /* the epoch being saved */

static bool take_perturb(uint32_t id);

bool kstate_take_perturb(uint32_t id) { return take_perturb(id); }

static bool take_perturb(uint32_t id) {
    if (g_perturb_id != id) return false;
    g_perturb_id = 0;
    return true;
}

void kstate_perturb_next_restore(uint32_t id) { g_perturb_id = id; }
uint64_t kstate_restored_now_ns(void) { return g_restored_now_ns; }

/* ---- procs ---- */

#define REGION_NAME 16

static void procs_save(ksf_writer_t* w, void* ctx) {
    (void)ctx;
    ksf_put_u32(w, proc_count());
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* p = proc_at(i);
        ksf_put_u32(w, (uint32_t)p->pid);
        ksf_put_u32(w, (uint32_t)p->ppid);
        ksf_put_u32(w, (uint32_t)p->state);
        ksf_put(w, p->name, MAX_PROCESS_NAME);
        ksf_put(w, p->exe_path, sizeof(p->exe_path));
        ksf_put_u32(w, p->exe_crc);
        ksf_put_u32(w, p->address_space->region_count);
        for (vm_region_t* r = p->address_space->regions; r; r = r->next) {
            char name[REGION_NAME];
            memset(name, 0, sizeof(name));
            memcpy(name, r->name, REGION_NAME - 1);
            ksf_put_u64(w, r->start_addr);
            ksf_put_u64(w, r->end_addr);
            ksf_put_u32(w, r->flags);
            ksf_put_u32(w, (uint32_t)r->type);
            ksf_put(w, name, REGION_NAME);
        }
    }
}

static int procs_restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    if (version != 1) return -1;
    vmm_switch_address_space(0);
    proc_destroy_all();
    uint32_t n = ksf_get_u32(r);
    for (uint32_t i = 0; i < n && r->ok; i++) {
        uint32_t pid = ksf_get_u32(r);
        uint32_t ppid = ksf_get_u32(r);
        uint32_t state = ksf_get_u32(r);
        char name[MAX_PROCESS_NAME];
        ksf_get(r, name, sizeof(name));
        name[MAX_PROCESS_NAME - 1] = 0;
        if (!r->ok || proc_by_pid(pid)) return -1;
        process_t* p = proc_alloc(pid, name);
        if (!p) return -1;
        p->ppid = (pid_t)ppid;
        p->state = (process_state_t)state;
        ksf_get(r, p->exe_path, sizeof(p->exe_path));
        p->exe_path[sizeof(p->exe_path) - 1] = 0;
        p->exe_crc = ksf_get_u32(r);
        uint32_t nreg = ksf_get_u32(r);
        for (uint32_t k = 0; k < nreg && r->ok; k++) {
            uint64_t start = ksf_get_u64(r), end = ksf_get_u64(r);
            uint32_t flags = ksf_get_u32(r), type = ksf_get_u32(r);
            char rname[REGION_NAME];
            ksf_get(r, rname, sizeof(rname));
            rname[REGION_NAME - 1] = 0;
            if (!r->ok || end <= start ||
                !vmm_create_region(p->address_space, start, end - start, flags,
                                   (vmm_region_type_t)type, rname)) {
                proc_destroy(p);
                return -1;
            }
        }
        proc_append(p);
    }
    return r->ok ? 0 : -1;
}

/* ---- cpuid ---- */

static void cpuid_save(ksf_writer_t* w, void* ctx) {
    (void)ctx;
    const cpuid_table_t* t = cpu_cpuid_table();
    ksf_put_u32(w, t->count);
    ksf_put_u32(w, t->faulting);
    ksf_put(w, t->e, t->count * (uint32_t)sizeof(cpuid_entry_t));
}

static int cpuid_restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    static cpuid_table_t t;
    if (version != 1) return -1;
    memset(&t, 0, sizeof(t));
    t.count = ksf_get_u32(r);
    t.faulting = ksf_get_u32(r);
    if (t.count > CPU_CPUID_MAX) return -1;
    ksf_get(r, t.e, t.count * (uint32_t)sizeof(cpuid_entry_t));
    if (!r->ok) return -1;
    cpu_cpuid_set_table(&t);
    return 0;
}

/* ---- clock ---- */

static void clock_save(ksf_writer_t* w, void* ctx) {
    (void)ctx;
    ksf_put(w, clock_base(), sizeof(clock_base_t));
    ksf_put_u64(w, clock_now_unrecorded());
}

static int clock_restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    clock_base_t b;
    if (version != 1 || !ksf_get(r, &b, sizeof(b))) return -1;
    g_restored_now_ns = ksf_get_u64(r);
    if (!r->ok) return -1;
    clock_set_base(&b);
    return 0;
}

/* ---- machine ---- */

static void machine_save(ksf_writer_t* w, void* ctx) {
    (void)ctx;
    process_t* cur = machine_current();
    const pending_entry_t* pe = machine_pending();
    ksf_put_u64(w, g_epoch);
    ksf_put_u32(w, cur ? (uint32_t)cur->pid : 0);
    ksf_put_u32(w, proc_next_pid());
    ksf_put_u64(w, pe->vector);
    ksf_put_u64(w, pe->error);
    ksf_put_u64(w, pe->cr2);
}

static int machine_restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    if (version != 1) return -1;
    (void)ksf_get_u64(r);                  /* the epoch: checked by the caller */
    uint32_t cur = ksf_get_u32(r);
    uint32_t next = ksf_get_u32(r);
    pending_entry_t pe;
    pe.vector = ksf_get_u64(r);
    pe.error = ksf_get_u64(r);
    pe.cr2 = ksf_get_u64(r);
    if (!r->ok || (cur && !proc_by_pid(cur))) return -1;
    proc_set_next_pid(next);
    machine_set_pending(&pe);
    machine_set_current(cur ? proc_by_pid(cur) : 0);
    return 0;
}

/* ---- probe: the format's own check ---- */

#define PROBE_VALUE 0x4C41504C41434531ULL   /* "LAPLACE1" */
static uint64_t g_probe = PROBE_VALUE;

static void probe_save(ksf_writer_t* w, void* ctx) { (void)ctx; ksf_put_u64(w, g_probe); }

static int probe_restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    if (version != 1) return -1;
    g_probe = ksf_get_u64(r);
    if (take_perturb(KS_ID_PROBE)) g_probe ^= 1;
    return r->ok ? 0 : -1;
}

static uint32_t probe_sum(void* ctx) {
    (void)ctx;
    return divergence_checksum(0, &g_probe, sizeof(g_probe));
}

/* ---- Registry ---- */

void kstate_init(void) {
    static const ksf_subsys_t builtin[] = {
        { KS_ID_PROCS,   "procs",   1, procs_save,   procs_restore,   0, 0 },
        { KS_ID_CPUID,   "cpuid",   1, cpuid_save,   cpuid_restore,   0, 0 },
        { KS_ID_CLOCK,   "clock",   1, clock_save,   clock_restore,   0, 0 },
        { KS_ID_MACHINE, "machine", 1, machine_save, machine_restore, 0, 0 },
        { KS_ID_PROBE,   "probe",   1, probe_save,   probe_restore,   probe_sum, 0 },
    };
    ksf_registry_init(&g_reg);
    for (uint32_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++) {
        if (ksf_register(&g_reg, &builtin[i]) != KSF_OK) panic("kstate: cannot register %s", builtin[i].name);
    }
}

int kstate_register(const ksf_subsys_t* sub) {
    return ksf_register(&g_reg, sub);
}

/* Component ids for subsystems with a checksum, in registration order. */
static uint32_t component_of(uint32_t index) {
    uint32_t c = KSTATE_COMPONENT_BASE;
    for (uint32_t i = 0; i < index; i++) {
        if (g_reg.subs[i].checksum) c++;
    }
    return c;
}

void kstate_register_checksums(void) {
    for (uint32_t i = 0; i < g_reg.count; i++) {
        if (!g_reg.subs[i].checksum) continue;
        uint32_t c = component_of(i);
        if (c >= DIVERGE_MAX_COMPONENTS) panic("kstate: too many checksummed subsystems");
        kdiverge_register(c, g_reg.subs[i].checksum, g_reg.subs[i].ctx);
    }
}

const char* kstate_component_name(uint32_t component) {
    switch (component) {
    case KDIVERGE_PROCTABLE:  return "process table";
    case KDIVERGE_SCHEDULER:  return "scheduler";
    case KDIVERGE_USER_PAGES: return "user pages";
    case KDIVERGE_CONTEXTS:   return "registers";
    default: break;
    }
    for (uint32_t i = 0; i < g_reg.count; i++) {
        if (g_reg.subs[i].checksum && component_of(i) == component) return g_reg.subs[i].name;
    }
    return "?";
}

const char* kstate_subsys_name(uint32_t id) {
    const ksf_subsys_t* s = ksf_find(&g_reg, id);
    return s ? s->name : "?";
}

uint8_t* kstate_encode(uint64_t epoch, uint32_t* size) {
    g_epoch = epoch;
    uint32_t n = ksf_measure(&g_reg);
    uint8_t* blob = (uint8_t*)kmalloc(n);
    if (!blob) return 0;
    if (ksf_encode(&g_reg, blob, n) != (int64_t)n) {
        kfree(blob);
        return 0;
    }
    *size = n;
    return blob;
}

bool kstate_blob_epoch(const uint8_t* blob, uint32_t size, uint64_t* epoch) {
    ksf_reader_t r;
    uint32_t version;
    if (ksf_validate(blob, size) != KSF_OK || !ksf_section(blob, size, KS_ID_MACHINE, &r, &version)) {
        return false;
    }
    *epoch = ksf_get_u64(&r);
    return r.ok;
}

int kstate_restore(const uint8_t* blob, uint32_t size, uint32_t* failed) {
    return ksf_restore(&g_reg, blob, size, failed);
}
