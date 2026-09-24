/* Laplace core: recording, keyframes, restore, and replay in the booted kernel
 * (#221, #222, #223, #224, #225, #231). See include/core/timetravel.h. */

#include "core/timetravel.h"
#include "core/machine.h"
#include "core/proc.h"
#include "core/mm.h"
#include "core/console.h"
#include "checkpoint.h"
#include "keyframe_store.h"
#include "journal_capture.h"
#include "replay_driver.h"
#include "replay_engine.h"
#include "rewind.h"
#include "reverse.h"
#include "revbreak.h"
#include "divergence.h"
#include "divergence_scan.h"
#include "time_record.h"
#include "entropy_record.h"
#include "sched_record.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

/* ---- On-disk layout ----
 *
 *   sector 0                      label (geometry + magic)
 *   [KF_BASE, KF_BASE + kf)       keyframe store: ring index + N regions
 *   [J_BASE, J_BASE + j)          journal ring: N regions
 */
#define LABEL_MAGIC       0x3145434150414C4CULL   /* "LLAPACE1" */
#define LABEL_VERSION     1
#define KF_BASE           8
#define KF_INDEX_SECTORS  4
#define KF_SLOT_SECTORS   2048   /* per region slot: up to 227 page records */
#define J_SLOT_SECTORS    640    /* per journal slot: up to 10224 events */

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t kf_slot_sectors;
    uint32_t j_slot_sectors;
    uint32_t kf_base;
    uint32_t j_base;
    uint32_t crc;
} tt_label_t;

/* Kernel-state blob captured with every keyframe (#224). */
#define KSTATE_TAG        0x4C50u               /* "LP" */
#define KSTATE_MAGIC      0x3145544154534B4CULL /* "LKSTATE1" */
#define KSTATE_MAX_REGIONS 8

typedef struct {
    uint64_t start, end;
    uint32_t flags, type;
    char     name[16];
} kstate_region_t;

typedef struct {
    uint32_t pid;
    uint32_t state;
    char     name[MAX_PROCESS_NAME];
    uint32_t nregions;
    uint32_t reserved;
    kstate_region_t regions[KSTATE_MAX_REGIONS];
} kstate_proc_t;

typedef struct {
    uint64_t magic;
    uint64_t epoch;
    uint32_t nproc;
    uint32_t current_pid;
    uint32_t next_pid;
    uint32_t reserved;
    uint64_t pending_vector;
    uint64_t pending_error;
    uint64_t pending_cr2;
    kstate_proc_t procs[LP_MAX_PROCS];
} kstate_t;

/* ---- State ---- */

static bool        g_ready;
static tt_config_t g_cfg;
static tt_stats_t  g_stats;
static uint32_t    g_ticks_since_kf;
static bool        g_wb_pending;
static uint32_t    g_wb_tick;          /* tick count when the keyframe was taken */
static uint32_t    g_ticks;

static bool        g_sealed;           /* the open epoch's journal is on disk */
static uint64_t    g_end_epoch;        /* the recording's stop point */
static uint64_t    g_end_step;

/* Epoch lengths, cached as journals are written or read. */
#define LEN_CACHE 64
static struct { uint64_t epoch; uint64_t len; bool valid; } g_len_cache[LEN_CACHE];

static uint8_t g_pagebuf[PAGE_SIZE] __attribute__((aligned(16)));

const tt_stats_t* tt_stats(void) { return &g_stats; }
bool tt_ready(void) { return g_ready; }

static void len_cache_put(uint64_t epoch, uint64_t len) {
    uint32_t i = (uint32_t)(epoch % LEN_CACHE);
    g_len_cache[i].epoch = epoch;
    g_len_cache[i].len = len;
    g_len_cache[i].valid = true;
}

bool tt_epoch_len(uint64_t epoch, uint64_t* len) {
    uint32_t i = (uint32_t)(epoch % LEN_CACHE);
    if (g_len_cache[i].valid && g_len_cache[i].epoch == epoch) {
        *len = g_len_cache[i].len;
        return true;
    }
    uint64_t l;
    if (replay_journal_epoch_len(epoch, &l) != 0) return false;
    len_cache_put(epoch, l);
    *len = l;
    return true;
}

/* ---- RAM block device (no IDE disk attached) ---- */

static uint64_t* g_ram_frames;
static uint32_t  g_ram_sectors;
static fat_block_device_t g_ram_dev;

static int ram_read(void* d, uint32_t s, uint32_t n, void* buf) {
    (void)d;
    if ((uint64_t)s + n > g_ram_sectors) return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sec = s + i;
        memcpy((uint8_t*)buf + (uint64_t)i * 512,
               (const void*)(g_ram_frames[sec / 8] + (sec % 8) * 512), 512);
    }
    return 0;
}

static int ram_write(void* d, uint32_t s, uint32_t n, const void* buf) {
    (void)d;
    if ((uint64_t)s + n > g_ram_sectors) return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sec = s + i;
        memcpy((void*)(g_ram_frames[sec / 8] + (sec % 8) * 512),
               (const uint8_t*)buf + (uint64_t)i * 512, 512);
    }
    return 0;
}

fat_block_device_t* tt_ram_device(uint32_t sectors) {
    uint32_t pages = (sectors + 7) / 8;
    g_ram_frames = (uint64_t*)kmalloc(pages * sizeof(uint64_t));
    if (!g_ram_frames) return 0;
    for (uint32_t i = 0; i < pages; i++) {
        g_ram_frames[i] = pmm_alloc();
        if (!g_ram_frames[i]) return 0;
    }
    g_ram_sectors = pages * 8;
    g_ram_dev.read_sectors = ram_read;
    g_ram_dev.write_sectors = ram_write;
    g_ram_dev.sector_size = 512;
    g_ram_dev.total_sectors = g_ram_sectors;
    g_ram_dev.private_data = 0;
    return &g_ram_dev;
}

uint32_t tt_required_sectors(uint32_t keyframes) {
    return KF_BASE + keyframe_store_total_sectors(KF_INDEX_SECTORS, keyframes, KF_SLOT_SECTORS)
         + journal_ring_total_sectors(keyframes, J_SLOT_SECTORS);
}

/* ---- Setup ---- */

static uint32_t label_crc(const tt_label_t* l) {
    return snapshot_crc32(0, l, (uint32_t)(sizeof(*l) - sizeof(l->crc)));
}

int tt_init(fat_block_device_t* dev, const tt_config_t* cfg, bool* had_keyframe) {
    *had_keyframe = false;
    g_cfg = *cfg;
    uint32_t capacity = cfg->keyframes;
    while (capacity > 2 && tt_required_sectors(capacity) > dev->total_sectors) capacity--;
    if (tt_required_sectors(capacity) > dev->total_sectors) {
        kprintf("timetravel: disk too small (%u sectors, need %u)\n",
                dev->total_sectors, tt_required_sectors(capacity));
        return -1;
    }
    g_cfg.keyframes = capacity;

    tt_label_t want;
    memset(&want, 0, sizeof(want));
    want.magic = LABEL_MAGIC;
    want.version = LABEL_VERSION;
    want.capacity = capacity;
    want.kf_slot_sectors = KF_SLOT_SECTORS;
    want.j_slot_sectors = J_SLOT_SECTORS;
    want.kf_base = KF_BASE;
    want.j_base = KF_BASE + keyframe_store_total_sectors(KF_INDEX_SECTORS, capacity, KF_SLOT_SECTORS);
    want.crc = label_crc(&want);

    static uint8_t sector[512];
    bool reuse = false;
    if (dev->read_sectors(dev, 0, 1, sector) == 0) {
        tt_label_t have;
        memcpy(&have, sector, sizeof(have));
        reuse = have.crc == label_crc(&have) && have.magic == want.magic &&
                have.version == want.version && have.capacity == want.capacity &&
                have.kf_slot_sectors == want.kf_slot_sectors &&
                have.j_slot_sectors == want.j_slot_sectors;
    }

    if (!reuse) {
        /* A blank or foreign disk: invalidate any stale keyframe index first,
         * then provision both stores and write the label last. */
        memset(sector, 0, sizeof(sector));
        for (uint32_t s = 0; s < KF_BASE + KF_INDEX_SECTORS; s++) {
            dev->write_sectors(dev, s, 1, sector);
        }
    }
    if (keyframe_store_arm(dev, KF_BASE, KF_INDEX_SECTORS, capacity, KF_SLOT_SECTORS)
            != KEYFRAME_STORE_OK) {
        kprintf("timetravel: keyframe store failed\n");
        return -1;
    }
    if (journal_capture_init(dev, want.j_base, J_SLOT_SECTORS, capacity, !reuse) != JOURNAL_OK) {
        kprintf("timetravel: journal ring failed\n");
        return -1;
    }
    if (!reuse) {
        memset(sector, 0, sizeof(sector));
        memcpy(sector, &want, sizeof(want));
        if (dev->write_sectors(dev, 0, 1, sector) != 0) return -1;
    }

    const keyframe_ring_t* ring = keyframe_store_ring(keyframe_store_get());
    *had_keyframe = reuse && keyframe_ring_count(ring) > 0;
    g_ready = true;
    kprintf("timetravel: store %s: %u keyframes x %u sectors, journal ring at %u, %u sectors total\n",
            reuse ? "reused" : "formatted", capacity, 1 + 2 * KF_SLOT_SECTORS, want.j_base,
            tt_required_sectors(capacity));
    return 0;
}

/* ---- Kernel-state blob ---- */

static void capture_kstate(uint64_t epoch) {
    static kstate_t ks;
    memset(&ks, 0, sizeof(ks));
    ks.magic = KSTATE_MAGIC;
    ks.epoch = epoch;
    ks.nproc = proc_count();
    process_t* cur = machine_current();
    ks.current_pid = cur ? (uint32_t)cur->pid : 0;
    ks.next_pid = proc_next_pid();
    ks.pending_vector = machine_pending()->vector;
    ks.pending_error = machine_pending()->error;
    ks.pending_cr2 = machine_pending()->cr2;
    for (uint32_t i = 0; i < ks.nproc; i++) {
        process_t* p = proc_at(i);
        kstate_proc_t* kp = &ks.procs[i];
        kp->pid = (uint32_t)p->pid;
        kp->state = (uint32_t)p->state;
        memcpy(kp->name, p->name, sizeof(kp->name));
        for (vm_region_t* r = p->address_space->regions; r && kp->nregions < KSTATE_MAX_REGIONS;
             r = r->next) {
            kstate_region_t* kr = &kp->regions[kp->nregions++];
            kr->start = r->start_addr;
            kr->end = r->end_addr;
            kr->flags = r->flags;
            kr->type = (uint32_t)r->type;
            memcpy(kr->name, r->name, sizeof(kr->name) - 1);
        }
    }
    uint32_t size = (uint32_t)((uint8_t*)&ks.procs[ks.nproc] - (uint8_t*)&ks);
    if (checkpoint_capture_kernel_blob(KSTATE_TAG, &ks, size) != CHECKPOINT_OK) {
        panic("timetravel: cannot capture kernel state");
    }
}

/* ---- Recording ---- */

static bool record_pressure(void) {
    const uint64_t* p64;
    const uint8_t* p8;
    if (ktime_values(&p64) >= KTIME_LOG_MAX * 3 / 4) return true;
    if (kentropy_bytes(&p8) >= KENTROPY_LOG_MAX * 3 / 4) return true;
    if (sched_record_points(machine_sched_record(), &p64) >= 3072) return true;
    return false;
}

static void do_writeback(void) {
    if (!g_wb_pending) return;
    uint64_t pages = 0;
    for (checkpoint_capture_t* c = checkpoint_captures(); c; c = c->next) {
        if (c->flags == 0) pages++;
    }
    int rc = checkpoint_writeback_keyframe();
    if (rc != CHECKPOINT_OK) panic("timetravel: keyframe writeback failed (%d)", rc);
    g_wb_pending = false;
    g_stats.writebacks++;
    g_stats.cow_captures += pages;
}

static void close_epoch(uint64_t epoch, uint64_t steps) {
    int rc = journal_capture_close_epoch(epoch, steps);
    if (rc != JOURNAL_OK) panic("timetravel: journal for epoch %lu failed (%d)", epoch, rc);
    len_cache_put(epoch, steps);
    g_stats.journals_written++;
}

static void take_keyframe(void) {
    uint64_t steps = machine_step();
    do_writeback();
    if (machine_epoch() > 0) close_epoch(machine_epoch(), steps);

    uint64_t e = checkpoint_take();
    if (e == 0) panic("timetravel: checkpoint_take failed");
    machine_set_position(e, 0);
    capture_kstate(e);

    sched_record_begin_epoch(machine_sched_record(), e);
    ktime_begin_epoch(e);
    kentropy_begin_epoch(e);

    g_wb_pending = true;
    g_wb_tick = g_ticks;
    g_ticks_since_kf = 0;
    g_sealed = false;
    g_stats.keyframes_taken++;
    if (g_cfg.stop_after && g_stats.keyframes_taken >= g_cfg.stop_after) machine_request_stop();
}

void tt_on_tick(void) {
    g_ticks++;
    g_ticks_since_kf++;
}

void tt_on_live_entry(void) {
    if (!g_ready) return;
    /* Writeback runs at a later entry than the take, so the processes write
     * pages in between and the COW hook preserves their keyframe images. */
    if (g_wb_pending && g_ticks != g_wb_tick) do_writeback();
    if (machine_epoch() == 0 || g_ticks_since_kf >= g_cfg.interval_ticks || record_pressure()) {
        take_keyframe();
    }
}

void tt_before_process_exit(process_t* p) {
    (void)p;
    /* The keyframe's clean pages are read from the live address space, which
     * is about to be freed. */
    if (machine_mode() == MACHINE_LIVE) do_writeback();
}

int tt_seal(void) {
    if (!g_ready || !machine_started() || machine_mode() != MACHINE_LIVE) return TT_ERR_STATE;
    do_writeback();
    close_epoch(machine_epoch(), machine_step());
    g_sealed = true;
    g_end_epoch = machine_epoch();
    g_end_step = machine_step();
    return TT_OK;
}

bool tt_window(tt_window_t* w) {
    keyframe_store_t* ks = keyframe_store_get();
    if (!g_ready || !ks) return false;
    const keyframe_ring_t* r = keyframe_store_ring(ks);
    if (!keyframe_ring_oldest(r, &w->oldest)) return false;
    keyframe_ring_newest(r, &w->newest);
    w->count = keyframe_ring_count(r);
    w->end_step = 0;
    tt_epoch_len(w->newest, &w->end_step);
    return true;
}

/* ---- Restore ---- */

typedef struct {
    uint32_t pid;
    process_context_t ctx;
} saved_ctx_t;

int tt_restore_keyframe(uint64_t epoch) {
    keyframe_store_t* ks = keyframe_store_get();
    if (!ks) return TT_ERR_STATE;
    snapshot_reader_t rd;
    snapshot_page_record_t rec;
    uint64_t sel = 0;
    if (keyframe_store_load_epoch(ks, epoch, &rd, &sel) != KEYFRAME_STORE_OK || sel != epoch) {
        return TT_ERR_RANGE;
    }

    /* Pass 1: the kernel-state blob and the saved registers. */
    static kstate_t kst;
    static saved_ctx_t ctxs[LP_MAX_PROCS];
    uint32_t nctx = 0;
    uint32_t blob_size = 0;
    memset(&kst, 0, sizeof(kst));
    rec.page_data = g_pagebuf;
    while (snapshot_reader_next(&rd, &rec) == SNAPSHOT_OK) {
        if ((rec.flags & CHECKPOINT_REC_KERNEL) && rec.pid == KSTATE_TAG) {
            uint32_t size = (uint32_t)(rec.virt_addr >> 32);
            uint32_t idx = (uint32_t)rec.virt_addr;
            uint32_t off = idx * PAGE_SIZE;
            if (size > sizeof(kst) || off >= size) return TT_ERR_RESTORE;
            uint32_t n = size - off < PAGE_SIZE ? size - off : PAGE_SIZE;
            memcpy((uint8_t*)&kst + off, g_pagebuf, n);
            blob_size = size;
        } else if (rec.flags & CHECKPOINT_REC_CONTEXT) {
            if (nctx >= LP_MAX_PROCS) return TT_ERR_RESTORE;
            ctxs[nctx].pid = rec.pid;
            memcpy(&ctxs[nctx].ctx, g_pagebuf, sizeof(process_context_t));
            nctx++;
        }
    }
    if (!blob_size || kst.magic != KSTATE_MAGIC || kst.epoch != epoch || kst.nproc > LP_MAX_PROCS) {
        return TT_ERR_RESTORE;
    }

    /* Discard the live machine and rebuild it from the keyframe. */
    vmm_switch_address_space(0);
    proc_destroy_all();
    for (uint32_t i = 0; i < kst.nproc; i++) {
        kstate_proc_t* kp = &kst.procs[i];
        process_t* p = proc_alloc(kp->pid, kp->name);
        if (!p) return TT_ERR_RESTORE;
        p->state = (process_state_t)kp->state;
        for (uint32_t r = 0; r < kp->nregions && r < KSTATE_MAX_REGIONS; r++) {
            kstate_region_t* kr = &kp->regions[r];
            vmm_create_region(p->address_space, kr->start, kr->end - kr->start, kr->flags,
                              (vmm_region_type_t)kr->type, kr->name);
        }
        for (uint32_t c = 0; c < nctx; c++) {
            if (ctxs[c].pid == kp->pid) p->context = ctxs[c].ctx;
        }
        proc_append(p);
    }
    proc_set_next_pid(kst.next_pid);

    /* Pass 2: the pages. */
    if (keyframe_store_load_epoch(ks, epoch, &rd, &sel) != KEYFRAME_STORE_OK) return TT_ERR_RESTORE;
    rec.page_data = g_pagebuf;
    while (snapshot_reader_next(&rd, &rec) == SNAPSHOT_OK) {
        if (rec.flags & (CHECKPOINT_REC_KERNEL | CHECKPOINT_REC_CONTEXT)) continue;
        process_t* p = proc_by_pid(rec.pid);
        if (!p) return TT_ERR_RESTORE;
        uint64_t phys = vmm_get_physical_addr(p->address_space, rec.virt_addr);
        if (!phys) {
            phys = pmm_alloc();
            uint32_t flags = PAGE_USER | ((rec.flags & CHECKPOINT_REC_READONLY) ? 0 : PAGE_WRITABLE);
            if (!phys || vmm_map_page(p->address_space, rec.virt_addr, phys, flags) != 0) {
                return TT_ERR_RESTORE;
            }
        }
        memcpy((void*)(phys & ~0xFFFULL), g_pagebuf, PAGE_SIZE);
    }

    pending_entry_t pe = { kst.pending_vector, kst.pending_error, kst.pending_cr2 };
    machine_set_pending(&pe);
    machine_set_current(proc_by_pid(kst.current_pid));
    machine_set_position(epoch, 0);
    machine_set_mode(MACHINE_REPLAY);
    g_stats.restores++;
    return TT_OK;
}

/* ---- Replay ---- */

int tt_drive(uint64_t epoch, uint64_t step, uint64_t insn, uint64_t epoch_len) {
    if (step > epoch_len || (step == epoch_len && insn > 0)) return TT_ERR_RANGE;
    if (machine_epoch() != epoch) return TT_ERR_STATE;
    machine_set_mode(MACHINE_REPLAY);
    machine_set_target(step, insn, epoch_len);
    uint64_t from = machine_step();
    machine_resume();
    machine_clear_target();
    g_stats.replayed_steps += machine_step() - from;
    const machine_stop_t* st = machine_last_stop();
    if (st->reason != STOP_TARGET) {
        kprintf("timetravel: replay of epoch %lu stopped (reason %d) at step %lu, wanted %lu+%lu\n",
                epoch, (int)st->reason, machine_step(), step, insn);
        return TT_ERR_REPLAY;
    }
    return TT_OK;
}

static bool in_window(uint64_t epoch) {
    keyframe_store_t* ks = keyframe_store_get();
    if (!ks) return false;
    uint32_t slot;
    uint64_t found;
    return keyframe_ring_find(keyframe_store_ring(ks), epoch, &slot, &found) && found == epoch;
}

int tt_goto(uint64_t epoch, uint64_t step, uint64_t insn) {
    if (!g_ready) return TT_ERR_STATE;
    if (machine_mode() == MACHINE_LIVE && !g_sealed) return TT_ERR_STATE;
    if (!in_window(epoch)) return TT_ERR_RANGE;
    uint64_t len;
    if (!tt_epoch_len(epoch, &len)) return TT_ERR_JOURNAL;
    if (step > len || (step == len && insn > 0)) return TT_ERR_RANGE;
    replay_set_insn_target(insn);
    int rc = krewind_to(epoch, step);
    replay_set_insn_target(0);
    if (rc != REWIND_OK) return rc == REWIND_ERR_OUT_OF_RANGE ? TT_ERR_RANGE : TT_ERR_REPLAY;
    if (step == 0) {
        /* The engine stops at a keyframe boundary without loading its
         * journal; load it so the machine can run forward from here. */
        if (replay_load_epoch(epoch) != 0) return TT_ERR_JOURNAL;
        if (insn > 0 && (rc = tt_drive(epoch, 0, insn, len)) != TT_OK) return rc;
    }
    kreverse_set_position(epoch, step);
    return TT_OK;
}

int tt_scan_epoch(uint64_t epoch, uint64_t limit, tt_visit_fn visit, void* ctx) {
    if (!g_ready) return TT_ERR_STATE;
    uint64_t len;
    if (!in_window(epoch) || !tt_epoch_len(epoch, &len)) return TT_ERR_RANGE;
    if (limit > len) limit = len;
    if (tt_restore_keyframe(epoch) != TT_OK) return TT_ERR_RESTORE;
    if (replay_load_epoch(epoch) != 0) return TT_ERR_JOURNAL;
    if (limit == 0) return TT_OK;
    visit(ctx, 0);
    if (limit == 1) return TT_OK;
    machine_set_entry_hook(visit, ctx);
    int rc = tt_drive(epoch, limit, 0, len);
    machine_set_entry_hook(0, 0);
    return rc;
}

int tt_segment_length(uint64_t epoch, uint64_t step, uint64_t* insns) {
    uint64_t len;
    if (!tt_epoch_len(epoch, &len) || step >= len) return TT_ERR_RANGE;
    int rc = tt_goto(epoch, step, 0);
    if (rc != TT_OK) return rc;
    machine_set_count_insns(true);
    rc = tt_drive(epoch, step + 1, 0, len);
    *insns = machine_counted_insns();
    machine_set_count_insns(false);
    if (rc == TT_OK) kreverse_set_position(epoch, step + 1);
    return rc;
}

int tt_resume_live(void) {
    if (!g_ready) return TT_ERR_STATE;
    if (machine_mode() == MACHINE_LIVE) return TT_OK;
    int rc = tt_goto(g_end_epoch, g_end_step, 0);
    if (rc != TT_OK) return rc;
    /* The wrappers now hold the newest epoch's journal with every input
     * consumed; switching to RECORD appends the continuation after it. */
    checkpoint_set_epoch(g_end_epoch);
    machine_set_mode(MACHINE_LIVE);
    g_sealed = false;
    return TT_OK;
}

void tt_perturb_next_replay(bool on) {
    replay_perturb_next(on);
}

/* ---- Verification ---- */

int tt_verify(tt_verify_t* out) {
    memset(out, 0, sizeof(*out));
    tt_window_t w;
    if (!tt_window(&w)) return TT_ERR_STATE;
    const keyframe_ring_t* ring = keyframe_store_ring(keyframe_store_get());
    /* Walk the retained keyframes oldest to newest. */
    uint64_t epoch = w.oldest;
    for (;;) {
        uint64_t len;
        if (tt_epoch_len(epoch, &len)) {
            uint32_t ids[KDIVERGE_COMPONENT_COUNT], sums[KDIVERGE_COMPONENT_COUNT], n = 0;
            if (replay_recorded_sums(epoch, ids, sums, KDIVERGE_COMPONENT_COUNT, &n) != 0 || n == 0) {
                return TT_ERR_JOURNAL;
            }
            int rc = tt_goto(epoch, len, 0);
            if (rc != TT_OK) return rc;
            kdiverge_reset();
            kdiverge_set_mode(DIVERGE_REPLAY);
            kdiverge_expect_pairs(epoch, ids, sums, n);
            bool ok = kdiverge_check_epoch();
            kdiverge_set_mode(DIVERGE_OFF);
            out->epochs_checked++;
            if (!ok) {
                const divergence_t* d = kdiverge_detector();
                out->epochs_diverged++;
                if (!out->diverged) {
                    out->diverged = true;
                    out->first_epoch = epoch;
                    out->first_component = d->diverged_component;
                    out->expected = d->diverged_expected;
                    out->actual = d->diverged_actual;
                }
            }
        }
        if (epoch == w.newest) break;
        /* Next retained keyframe after `epoch`. */
        uint64_t next = w.newest;
        for (uint32_t i = 0; i < KEYFRAME_RING_MAX; i++) {
            const keyframe_slot_t* s = &ring->slots[i];
            if (s->valid && s->epoch > epoch && s->epoch < next) next = s->epoch;
        }
        epoch = next;
    }
    return TT_OK;
}

/* ---- Boot resume ---- */

int tt_resume_from_store(void) {
    tt_window_t w;
    if (!tt_window(&w)) return TT_ERR_STATE;
    int rc = tt_restore_keyframe(w.newest);
    if (rc != TT_OK) return rc;
    /* Continue live from the keyframe: a new timeline from (newest, 0). */
    checkpoint_set_epoch(w.newest);
    machine_set_mode(MACHINE_LIVE);
    sched_record_begin_epoch(machine_sched_record(), w.newest);
    ktime_begin_epoch(w.newest);
    kentropy_begin_epoch(w.newest);
    g_ticks_since_kf = 0;
    g_sealed = false;
    return TT_OK;
}

/* ---- Binding the time-travel verbs (#226) ---- */

static uint64_t epoch_len_cb(void* ctx, uint64_t epoch) {
    (void)ctx;
    uint64_t len;
    return tt_epoch_len(epoch, &len) ? len : 0;
}

typedef struct {
    revbreak_visit_fn visit;
    void*             vctx;
    uint64_t          epoch;
} scan_pack_t;

static void scan_visit(void* c, uint64_t step) {
    scan_pack_t* p = (scan_pack_t*)c;
    reverse_pos_t pos = { p->epoch, step };
    p->visit(p->vctx, pos);
}

/* revbreak's scanner: one forward re-execution of the epoch per search step. */
static int scan_cb(void* sctx, uint64_t epoch, uint64_t limit,
                   revbreak_visit_fn visit, void* vctx) {
    (void)sctx;
    scan_pack_t p = { visit, vctx, epoch };
    return tt_scan_epoch(epoch, limit, scan_visit, &p) == TT_OK ? 0 : -1;
}

void tt_bind_verbs(void) {
    replay_kernel_bind();
    krewind_bind(keyframe_store_ring(keyframe_store_get()), replay_kernel_engine());
    kreverse_bind(krewind_ctx(), epoch_len_cb, 0);
    krevbreak_bind(kreverse_ctx());
    krevbreak_set_scanner(scan_cb, 0);
}
