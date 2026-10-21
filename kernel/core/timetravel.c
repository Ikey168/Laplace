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
#include "core/kjournal.h"
#include "core/clock.h"
#include "core/vm.h"
#include "core/kstate.h"
#include "core/kfpages.h"
#include "core/kio.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

/* ---- On-disk layout ----
 *
 *   sector 0                      label (geometry + magic)
 *   [KF_BASE, KF_BASE + kf)       keyframe store: ring index + N regions, each
 *                                 a keyframe's kernel state, registers, and
 *                                 page manifest
 *   [J_BASE, J_BASE + j)          journal ring: N regions
 *   [POOL_BASE, end)              page pool: one slot (8 sectors) per distinct
 *                                 page the retained keyframes hold (#244)
 *
 * Region sizes follow the disk's: a keyframe region gets 1/16 of the disk's
 * share per keyframe (512..4096 sectors), the pool the rest.
 */
#define LABEL_MAGIC       0x3145434150414C4CULL   /* "LLAPACE1" */
#define LABEL_VERSION     7   /* 2: CPUID (#241); 3: clock (#238); 4: executables (#240);
                                 5: region list (#239); 6: sectioned kernel state (#243);
                                 7: page pool (#244) */
#define KF_BASE           8
#define KF_INDEX_SECTORS  4
#define KF_SLOT_MIN       512    /* per keyframe region (x2, double-buffered) */
#define KF_SLOT_MAX       4096
#define J_SLOT_MIN        640    /* per journal slot: 10224 events ... */
#define J_SLOT_MAX        2560   /* ... to 40944 */
#define POOL_MIN_SLOTS    1024
#define KFP_TAG           0x4D50u   /* "PM": the page manifest blob */

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t kf_slot_sectors;
    uint32_t j_slot_sectors;
    uint32_t kf_base;
    uint32_t j_base;
    uint32_t pool_base;
    uint32_t pool_slots;
    uint32_t crc;
} tt_label_t;

typedef struct {
    uint32_t slot;          /* keyframe region sectors */
    uint32_t jslot;         /* journal slot sectors */
    uint32_t j_base;
    uint32_t pool_base;
    uint32_t pool_slots;
} geom_t;

static uint32_t meta_sectors(uint32_t keyframes, uint32_t slot, uint32_t jslot) {
    return KF_BASE + keyframe_store_total_sectors(KF_INDEX_SECTORS, keyframes, slot)
         + journal_ring_total_sectors(keyframes, jslot);
}

static bool geometry(uint32_t total, uint32_t keyframes, geom_t* g) {
    uint32_t slot = total / (keyframes * 16);
    if (slot < KF_SLOT_MIN) slot = KF_SLOT_MIN;
    if (slot > KF_SLOT_MAX) slot = KF_SLOT_MAX;
    slot &= ~7u;
    uint32_t jslot = total / (keyframes * 128);
    if (jslot < J_SLOT_MIN) jslot = J_SLOT_MIN;
    if (jslot > J_SLOT_MAX) jslot = J_SLOT_MAX;
    jslot &= ~7u;
    uint32_t base = (meta_sectors(keyframes, slot, jslot) + 7) & ~7u;
    if (base > total || (total - base) / 8 < POOL_MIN_SLOTS) return false;
    g->slot = slot;
    g->jslot = jslot;
    g->j_base = KF_BASE + keyframe_store_total_sectors(KF_INDEX_SECTORS, keyframes, slot);
    g->pool_base = base;
    g->pool_slots = (total - base) / 8;
    return true;
}

/* Kernel-state blob captured with every keyframe (#224, #243): one section per
 * kernel subsystem, see include/core/kstate.h. */
#define KSTATE_TAG        0x4C50u               /* "LP" */

/* ---- State ---- */

static bool        g_ready;
static tt_config_t g_cfg;
static tt_stats_t  g_stats;
static uint32_t    g_ticks_since_kf;
static bool        g_wb_pending;
static uint32_t    g_wb_tick;          /* tick count when the keyframe was taken */
static uint32_t    g_ticks;

static bool        g_sealed;           /* the open epoch's journal is on disk */
static bool        g_force_keyframe;   /* take one at the next live entry */
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

/* Frames are allocated on first write; an unwritten sector reads as zeros. */
static int ram_read(void* d, uint32_t s, uint32_t n, void* buf) {
    (void)d;
    if ((uint64_t)s + n > g_ram_sectors) return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sec = s + i;
        uint64_t f = g_ram_frames[sec / 8];
        if (f) memcpy((uint8_t*)buf + (uint64_t)i * 512, P2V(f + (sec % 8) * 512), 512);
        else memset((uint8_t*)buf + (uint64_t)i * 512, 0, 512);
    }
    return 0;
}

static int ram_write(void* d, uint32_t s, uint32_t n, const void* buf) {
    (void)d;
    if ((uint64_t)s + n > g_ram_sectors) return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sec = s + i;
        if (!g_ram_frames[sec / 8] && !(g_ram_frames[sec / 8] = pmm_alloc())) return -1;
        memcpy(P2V(g_ram_frames[sec / 8] + (sec % 8) * 512),
               (const uint8_t*)buf + (uint64_t)i * 512, 512);
    }
    return 0;
}

fat_block_device_t* tt_ram_device(uint32_t sectors) {
    uint32_t pages = (sectors + 7) / 8;
    g_ram_frames = (uint64_t*)kzalloc((uint64_t)pages * sizeof(uint64_t));
    if (!g_ram_frames) return 0;
    g_ram_sectors = pages * 8;
    g_ram_dev.read_sectors = ram_read;
    g_ram_dev.write_sectors = ram_write;
    g_ram_dev.sector_size = 512;
    g_ram_dev.total_sectors = g_ram_sectors;
    g_ram_dev.private_data = 0;
    return &g_ram_dev;
}

uint32_t tt_required_sectors(uint32_t keyframes, uint32_t pool_mib) {
    /* Enough for the largest keyframe regions plus the pool asked for. */
    uint64_t n = ((meta_sectors(keyframes, KF_SLOT_MAX, J_SLOT_MAX) + 7) & ~7u) +
                 (uint64_t)pool_mib * 2048;
    return n > 0xFFFFFFF8u ? 0xFFFFFFF8u : (uint32_t)n;
}

/* ---- Setup ---- */

static uint32_t label_crc(const tt_label_t* l) {
    return snapshot_crc32(0, l, (uint32_t)__builtin_offsetof(tt_label_t, crc));
}

static uint8_t* load_blob(uint64_t epoch, uint32_t tag, uint32_t* len);

int tt_init(fat_block_device_t* dev, const tt_config_t* cfg, bool* had_keyframe) {
    *had_keyframe = false;
    g_cfg = *cfg;
    uint32_t capacity = cfg->keyframes;
    geom_t g;
    while (capacity > 2 && !geometry(dev->total_sectors, capacity, &g)) capacity--;
    if (!geometry(dev->total_sectors, capacity, &g)) {
        kprintf("timetravel: disk too small (%u sectors)\n", dev->total_sectors);
        return -1;
    }
    g_cfg.keyframes = capacity;

    tt_label_t want;
    memset(&want, 0, sizeof(want));
    want.magic = LABEL_MAGIC;
    want.version = LABEL_VERSION;
    want.capacity = capacity;
    want.kf_slot_sectors = g.slot;
    want.j_slot_sectors = g.jslot;
    want.kf_base = KF_BASE;
    want.j_base = g.j_base;
    want.pool_base = g.pool_base;
    want.pool_slots = g.pool_slots;
    want.crc = label_crc(&want);

    static uint8_t sector[512];
    bool reuse = false;
    if (dev->read_sectors(dev->private_data, 0, 1, sector) == 0) {
        tt_label_t have;
        memcpy(&have, sector, sizeof(have));
        reuse = have.crc == label_crc(&have) && have.magic == want.magic &&
                have.version == want.version && have.capacity == want.capacity &&
                have.kf_slot_sectors == want.kf_slot_sectors &&
                have.j_slot_sectors == want.j_slot_sectors && have.pool_base == want.pool_base &&
                have.pool_slots == want.pool_slots;
    }

    if (!reuse) {
        /* A blank or foreign disk: invalidate any stale keyframe index first,
         * then provision the stores and write the label last. */
        memset(sector, 0, sizeof(sector));
        for (uint32_t s = 0; s < KF_BASE + KF_INDEX_SECTORS; s++) {
            dev->write_sectors(dev->private_data, s, 1, sector);
        }
    }
    if (keyframe_store_arm(dev, KF_BASE, KF_INDEX_SECTORS, capacity, g.slot) != KEYFRAME_STORE_OK) {
        kprintf("timetravel: keyframe store failed\n");
        return -1;
    }
    if (journal_capture_init(dev, want.j_base, g.jslot, capacity, !reuse) != JOURNAL_OK) {
        kprintf("timetravel: journal ring failed\n");
        return -1;
    }
    if (kfp_init(dev, g.pool_base, g.pool_slots) != 0) {
        kprintf("timetravel: no memory for the page pool's counts\n");
        return -1;
    }
    checkpoint_set_mark_fn(kfp_mark_space);
    /* The extended journal (inputs, traces) gets what the slot holds beyond
     * room for the preemption points and checksums. */
    kj_set_limit((g.jslot - 1) * 16 - 4096);
    if (!reuse) {
        memset(sector, 0, sizeof(sector));
        memcpy(sector, &want, sizeof(want));
        if (dev->write_sectors(dev->private_data, 0, 1, sector) != 0) {
            kprintf("timetravel: cannot write the disk label\n");
            return -1;
        }
    }

    const keyframe_ring_t* ring = keyframe_store_ring(keyframe_store_get());
    g_ready = true;
    if (reuse && keyframe_ring_count(ring) > 0) {
        /* The retained keyframes' manifests say which pool slots are in use. */
        tt_window_t w;
        bool all = tt_window(&w);
        for (uint64_t e = w.oldest; all && e <= w.newest; e++) {
            uint32_t slot;
            uint64_t found;
            if (!keyframe_ring_find(ring, e, &slot, &found) || found != e) continue;
            uint32_t len = 0;
            uint8_t* blob = load_blob(e, KFP_TAG, &len);
            if (!blob || kfp_adopt(e, blob, len) != 0) all = false;
            if (blob) kfree(blob);
            kio_adopt_journal(e);         /* its input payload pages (#242) */
        }
        if (!all) kprintf("timetravel: a retained keyframe's page manifest is unreadable\n");
        *had_keyframe = all;
    }
    kprintf("timetravel: store %s: %u keyframes x %u sectors, journal ring at %u (%u-sector "
            "slots), page pool of %u slots (%u MiB) at %u\n",
            reuse ? "reused" : "formatted", capacity, 1 + 2 * g.slot, want.j_base, g.jslot,
            g.pool_slots, g.pool_slots / 256, g.pool_base);
    return 0;
}

/* ---- Kernel-state blob ---- */

static void capture_kstate(uint64_t epoch) {
    uint32_t size = 0;
    uint8_t* blob = kstate_encode(epoch, &size);
    if (!blob || checkpoint_capture_kernel_blob(KSTATE_TAG, blob, size) != CHECKPOINT_OK) {
        panic("timetravel: cannot capture kernel state");
    }
    kfree(blob);
}

/* ---- Recording ---- */

#define KIO_EPOCH_BUDGET (256ULL << 20)   /* input payload bytes per epoch */

static bool record_pressure(void) {
    const uint64_t* p64;
    if (sched_record_points(machine_sched_record(), &p64) >= 3072) return true;
    if (kj_count() >= kj_limit() * 3 / 4) return true;
    if (kio_epoch_bytes() >= KIO_EPOCH_BUDGET) return true;
    return false;
}

static void do_writeback(void) {
    if (!g_wb_pending) return;
    uint64_t pages = 0;
    for (checkpoint_capture_t* c = checkpoint_captures(); c; c = c->next) {
        if (c->flags == 0) pages++;
    }
    int rc = kfp_writeback();
    if (rc != CHECKPOINT_OK) panic("timetravel: keyframe writeback failed (%d)", rc);
    g_wb_pending = false;
    g_stats.writebacks++;
    g_stats.cow_captures += pages;
}

static void close_epoch(uint64_t epoch, uint64_t steps) {
    kio_close_epoch(epoch);       /* the inputs' last page, and who owns the pages */
    int rc = journal_capture_close_epoch(epoch, steps);
    if (rc != JOURNAL_OK) panic("timetravel: journal for epoch %lu failed (%d)", epoch, rc);
    len_cache_put(epoch, steps);
    g_stats.journals_written++;
}

static void take_keyframe(void) {
    uint64_t steps = machine_step();
    machine_fpu_flush();
    do_writeback();
    if (machine_epoch() > 0) close_epoch(machine_epoch(), steps);

    kfp_begin_take();             /* which pages changed since the last keyframe */
    uint64_t e = checkpoint_take();
    if (e == 0) panic("timetravel: checkpoint_take failed");
    kfp_end_take(e, KFP_TAG);
    machine_set_position(e, 0);
    capture_kstate(e);

    sched_record_begin_epoch(machine_sched_record(), e);
    ktime_begin_epoch(e);
    kentropy_begin_epoch(e);
    kj_begin_epoch(e);
    kio_begin_epoch(e);

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

bool tt_entry_wanted(void) {
    if (!g_ready) return false;
    return (g_wb_pending && g_ticks != g_wb_tick) || g_ticks_since_kf >= g_cfg.interval_ticks ||
           g_force_keyframe;
}

void tt_on_live_entry(void) {
    if (!g_ready) return;
    /* Writeback runs at a later entry than the take, so the processes write
     * pages in between and the COW hook preserves their keyframe images. */
    if (g_wb_pending && g_ticks != g_wb_tick) do_writeback();
    /* A keyframe when the interval elapses, or early when the epoch has
     * dirtied enough pages that a restore would replay a lot of writes (#244). */
    if (machine_epoch() == 0 || g_ticks_since_kf >= g_cfg.interval_ticks || record_pressure() ||
        g_force_keyframe || (g_cfg.dirty_pages && kfp_dirty() >= g_cfg.dirty_pages)) {
        g_force_keyframe = false;
        take_keyframe();
    }
}

void tt_before_process_exit(process_t* p) {
    (void)p;
    /* The keyframe's clean pages are read from the live address space, which
     * is about to be freed. */
    if (machine_mode() == MACHINE_LIVE) do_writeback();
}

void tt_before_mapping_change(void) {
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

static bool g_perturb_xmm;
void tt_perturb_next_restore(bool on) { g_perturb_xmm = on; }

/* A kernel blob of the keyframe for `epoch`, reassembled; kmalloc'd. */
static uint8_t* load_blob(uint64_t epoch, uint32_t tag, uint32_t* len) {
    keyframe_store_t* ks = keyframe_store_get();
    snapshot_reader_t rd;
    snapshot_page_record_t rec;
    uint64_t sel = 0;
    if (!ks || keyframe_store_load_epoch(ks, epoch, &rd, &sel) != KEYFRAME_STORE_OK || sel != epoch) {
        return 0;
    }
    uint8_t* blob = 0;
    uint32_t blob_size = 0;
    rec.page_data = g_pagebuf;
    while (snapshot_reader_next(&rd, &rec) == SNAPSHOT_OK) {
        if (!(rec.flags & CHECKPOINT_REC_KERNEL) || rec.pid != tag) continue;
        uint32_t size = (uint32_t)(rec.virt_addr >> 32);
        uint32_t off = (uint32_t)rec.virt_addr * PAGE_SIZE;
        if (!blob) {
            blob = (uint8_t*)kmalloc(size);
            if (!blob) return 0;
            blob_size = size;
        }
        if (size != blob_size || off >= size) {
            kfree(blob);
            return 0;
        }
        memcpy(blob + off, g_pagebuf, size - off < PAGE_SIZE ? size - off : PAGE_SIZE);
    }
    *len = blob_size;
    return blob;
}

int tt_restore_keyframe(uint64_t epoch) {
    keyframe_store_t* ks = keyframe_store_get();
    if (!ks) return TT_ERR_STATE;
    if (!kfp_has(epoch)) return TT_ERR_RANGE;

    /* The kernel-state blob. */
    uint32_t blob_size = 0;
    uint8_t* blob = load_blob(epoch, KSTATE_TAG, &blob_size);
    uint64_t blob_epoch = 0;
    if (!blob || !kstate_blob_epoch(blob, blob_size, &blob_epoch) || blob_epoch != epoch) {
        if (blob) kfree(blob);
        return TT_ERR_RESTORE;
    }

    /* Keep the frames of unchanged pages, then discard the live machine and
     * rebuild it from the keyframe. */
    kfp_harvest();
    uint32_t failed = 0;
    int krc = kstate_restore(blob, blob_size, &failed);
    kfree(blob);
    if (krc != KSF_OK) {
        kprintf("timetravel: keyframe %lu: kernel state section %u (%s) did not restore (%d)\n",
                epoch, failed, kstate_subsys_name(failed), krc);
        return TT_ERR_RESTORE;
    }

    /* The registers, then the pages. */
    snapshot_reader_t rd;
    snapshot_page_record_t rec;
    uint64_t sel = 0;
    if (keyframe_store_load_epoch(ks, epoch, &rd, &sel) != KEYFRAME_STORE_OK) return TT_ERR_RESTORE;
    rec.page_data = g_pagebuf;
    while (snapshot_reader_next(&rd, &rec) == SNAPSHOT_OK) {
        if (!(rec.flags & CHECKPOINT_REC_CONTEXT)) continue;
        process_t* p = proc_by_pid(rec.pid);
        if (!p) return TT_ERR_RESTORE;
        proc_context_load(p, g_pagebuf);
        if (g_perturb_xmm && p == proc_at(0)) {
            /* Test hook: flip a bit of XMM15, which programs rarely touch. */
            g_perturb_xmm = false;
            proc_arch(p)->fpu[FPU_XMM_OFFSET + 15 * 16] ^= 0x01;
        }
    }
    if (kfp_map(epoch) != 0) {
        kprintf("timetravel: keyframe %lu: its pages did not restore\n", epoch);
        return TT_ERR_RESTORE;
    }

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
    /* An epoch's positions are its entries 0 .. len; (epoch, len) is where it
     * ends (the next keyframe, or the stop point of the newest epoch). */
    if (limit > len + 1) limit = len + 1;
    if (tt_restore_keyframe(epoch) != TT_OK) return TT_ERR_RESTORE;
    if (replay_load_epoch(epoch) != 0) return TT_ERR_JOURNAL;
    if (limit == 0) return TT_OK;
    visit(ctx, 0);
    if (limit == 1) return TT_OK;
    /* The hook fires at every replayed entry, including the one the drive
     * stops at: driving to limit - 1 visits 1 .. limit - 1. */
    machine_set_entry_hook(visit, ctx);
    int rc = tt_drive(epoch, limit - 1, 0, len);
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
    kio_continue();
    g_sealed = false;
    return TT_OK;
}

void tt_perturb_next_replay(bool on) {
    replay_perturb_next(on);
}

/* ---- Verification ---- */

/* Instructions whose result the recorder cannot reproduce (#241): RDRAND and
 * RDSEED cannot be trapped, RDPID reads TSC_AUX, and CPUID runs natively when
 * the CPU has no CPUID faulting. Decodes the instruction at `rip` past legacy
 * and REX prefixes; returns its name, or NULL. */
static const char* leak_at(process_t* p, uint64_t rip) {
    uint8_t b[8];
    if (!uaccess_read(p->address_space, rip, b, sizeof(b)) &&
        !uaccess_read(p->address_space, rip, b, 4)) {
        return 0;
    }
    uint32_t i = 0;
    bool rep = false;
    while (i < 4 && (b[i] == 0x66 || b[i] == 0xF2 || b[i] == 0xF3 || (b[i] & 0xF0) == 0x40)) {
        if (b[i] == 0xF3) rep = true;
        i++;
    }
    if (b[i] != 0x0F) return 0;
    uint8_t op = b[i + 1], modrm = b[i + 2];
    if (op == 0xA2 && !cpu_cpuid_faulting()) return "cpuid";
    if (op == 0xC7 && (modrm >> 6) == 3) {
        uint32_t reg = (modrm >> 3) & 7;
        if (reg == 6) return "rdrand";
        if (reg == 7) return rep ? "rdpid" : "rdseed";
    }
    return 0;
}

typedef struct {
    tt_verify_t* out;
    uint64_t     budget;
} leak_scan_t;

static void leak_visit(void* c, process_t* p, uint64_t rip) {
    leak_scan_t* ls = (leak_scan_t*)c;
    if (ls->out->leak_found) return;
    if (ls->budget == 0) {
        machine_stop_counting();   /* the window is too long to search: run the rest */
        return;
    }
    ls->budget--;
    const char* what = leak_at(p, rip);
    if (!what) return;
    machine_stop_counting();
    ls->out->leak_found = true;
    ls->out->leak_what = what;
    ls->out->leak_pid = (uint32_t)p->pid;
    ls->out->leak_rip = rip;
    ls->out->leak_step = machine_step();
    ls->out->leak_insn = machine_insn();
}

/* The first epoch diverged: bound where (from the register trace) and look
 * for an instruction the recorder cannot replay in that window. */
static void diagnose(uint64_t epoch, uint64_t len, tt_verify_t* out) {
    uint64_t bad, lo = 0, hi = len;
    if (machine_trace_mismatch(&bad)) {
        /* The sample KJ_TRACE_EVERY entries earlier still matched. */
        hi = bad;
        lo = bad >= KJ_TRACE_EVERY ? bad - KJ_TRACE_EVERY : 0;
    }
    out->diag_step_lo = lo;
    out->diag_step_hi = hi;
    if (tt_goto(epoch, lo, 0) != TT_OK) return;
    leak_scan_t ls = { out, 2000000 };
    machine_set_insn_hook(leak_visit, &ls);
    machine_set_count_insns(true);
    tt_drive(epoch, hi, 0, len);
    machine_set_count_insns(false);
    machine_set_insn_hook(0, 0);
}

/* The replay made an input call the recording does not have (#242), or could
 * not find a preemption point (#245): diverged at that step, in the inputs. */
static void inputs_diverged(tt_verify_t* out, uint64_t epoch, const kio_mismatch_t* mis) {
    out->epochs_diverged++;
    if (out->diverged) return;
    out->diverged = true;
    out->first_epoch = epoch;
    out->first_component = KDIVERGE_INPUTS;
    out->expected = mis->exhausted ? 0 : (mis->rec_kind << 16) | mis->rec_nr;
    out->actual = (mis->kind << 16) | mis->nr;
    out->diag_step_lo = mis->step;
    out->diag_step_hi = mis->step;
}

static int verify_window(tt_verify_t* out);

int tt_verify(tt_verify_t* out) {
    /* A preemption point the replay cannot find is noted and taken at the
     * next entry, so the epoch still runs to its end and its checksums name
     * what differs (#245). */
    machine_set_lenient(true);
    int rc = verify_window(out);
    machine_set_lenient(false);
    return rc;
}

static int verify_window(tt_verify_t* out) {
    memset(out, 0, sizeof(*out));
    tt_window_t w;
    if (!tt_window(&w)) return TT_ERR_STATE;
    const keyframe_ring_t* ring = keyframe_store_ring(keyframe_store_get());
    /* Walk the retained keyframes oldest to newest. */
    uint64_t epoch = w.oldest;
    for (;;) {
        uint64_t len;
        if (tt_epoch_len(epoch, &len)) {
            uint32_t ids[DIVERGE_MAX_COMPONENTS], sums[DIVERGE_MAX_COMPONENTS], n = 0;
            if (replay_recorded_sums(epoch, ids, sums, DIVERGE_MAX_COMPONENTS, &n) != 0 || n == 0) {
                return TT_ERR_JOURNAL;
            }
            kio_clear_mismatch();
            int rc = tt_goto(epoch, len, 0);
            const kio_mismatch_t* mis = kio_mismatch();
            if (rc != TT_OK && mis->valid) {
                out->epochs_checked++;
                inputs_diverged(out, epoch, mis);
                kio_clear_mismatch();
                goto next_epoch;
            }
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
                    diagnose(epoch, len, out);
                }
            } else if (mis->valid) {
                inputs_diverged(out, epoch, mis);   /* a preemption point, and nothing else differs */
            }
            kio_clear_mismatch();
        }
    next_epoch:
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
    /* The recording's programs must be the ones this boot has (#240): the
     * debuggers read their symbols from these binaries. */
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* p = proc_at(i);
        const lp_image_t* img = p->exe_path[0] ? proc_find_image(p->exe_path) : 0;
        if (p->exe_path[0] && (!img || proc_image_crc(img) != p->exe_crc)) {
            kprintf("timetravel: the recording ran %s, which this boot %s\n", p->exe_path,
                    img ? "has in a different version" : "does not have");
            proc_destroy_all();
            return TT_ERR_RESTORE;
        }
    }
    /* Continue live from the keyframe: a new timeline from (newest, 0). This
     * boot's TSC is not the recording's: keep the recorded clocks running from
     * the keyframe's time, and record the new base in a keyframe at once. */
    clock_reanchor(kstate_restored_now_ns());
    g_force_keyframe = true;
    checkpoint_set_epoch(w.newest);
    machine_set_mode(MACHINE_LIVE);
    sched_record_begin_epoch(machine_sched_record(), w.newest);
    ktime_begin_epoch(w.newest);
    kentropy_begin_epoch(w.newest);
    kj_begin_epoch(w.newest);
    kio_begin_epoch(w.newest);
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

/* ---- Instruction-level navigation (#227, #228) ---- */

void tt_position(tt_pos_t* p) {
    p->epoch = machine_epoch();
    p->step = machine_step();
    p->insn = machine_insn();
}

static bool pos_before(const tt_pos_t* a, const tt_pos_t* b) {
    if (a->epoch != b->epoch) return a->epoch < b->epoch;
    if (a->step != b->step) return a->step < b->step;
    return a->insn < b->insn;
}

static bool neighbour_epoch(uint64_t epoch, bool newer, uint64_t* out) {
    keyframe_store_t* ks = keyframe_store_get();
    if (!ks) return false;
    const keyframe_ring_t* r = keyframe_store_ring(ks);
    bool found = false;
    for (uint32_t i = 0; i < KEYFRAME_RING_MAX; i++) {
        const keyframe_slot_t* s = &r->slots[i];
        if (!s->valid) continue;
        if (newer ? (s->epoch > epoch && (!found || s->epoch < *out))
                  : (s->epoch < epoch && (!found || s->epoch > *out))) {
            *out = s->epoch;
            found = true;
        }
    }
    return found;
}

bool tt_at_end(void) {
    if (machine_mode() == MACHINE_LIVE) return true;
    return machine_epoch() == g_end_epoch && machine_step() == g_end_step &&
           machine_at_entry();
}

int tt_goto_pos(const tt_pos_t* p) {
    return tt_goto(p->epoch, p->step, p->insn);
}

/* The machine sits at the end of epoch `epoch` (its last entry), which is the
 * state keyframe `next` captured: continue in `next` without restoring. */
static int cross_into(uint64_t next) {
    if (replay_load_epoch(next) != 0) return TT_ERR_JOURNAL;
    machine_set_position(next, 0);
    kreverse_set_position(next, 0);
    return TT_OK;
}

#define TT_SEGMENT_ENDED 10   /* internal: the target lay past its segment's end */

/* Run forward to (epoch, step, insn), continuing from the current state when
 * the machine is replaying that epoch at or before the target. */
static int advance_to(uint64_t epoch, uint64_t step, uint64_t insn) {
    tt_pos_t c, t = { epoch, step, insn };
    tt_position(&c);
    uint64_t len;
    if (!tt_epoch_len(epoch, &len)) return TT_ERR_JOURNAL;
    if (machine_mode() != MACHINE_REPLAY || c.epoch != epoch || pos_before(&t, &c)) {
        return tt_goto(epoch, step, insn);
    }
    if (!pos_before(&c, &t)) return TT_OK;   /* already there */
    uint64_t from = machine_step();
    machine_set_target(step, insn, len);
    machine_resume();
    machine_clear_target();
    g_stats.replayed_steps += machine_step() - from;
    const machine_stop_t* st = machine_last_stop();
    kreverse_set_position(machine_epoch(), machine_step());
    if (st->reason == STOP_TARGET) return TT_OK;
    if (st->reason == STOP_END && machine_at_entry() && insn > 0 && machine_step() == step + 1) {
        return TT_SEGMENT_ENDED;
    }
    return TT_ERR_REPLAY;
}

int tt_stepi(void) {
    if (machine_mode() != MACHINE_REPLAY) return tt_at_end() ? TT_END : TT_ERR_STATE;
    if (tt_at_end()) return TT_END;
    tt_pos_t p;
    tt_position(&p);
    uint64_t len;
    if (!tt_epoch_len(p.epoch, &len)) return TT_ERR_JOURNAL;
    if (machine_at_entry() && p.step == len) {
        uint64_t next;
        if (!neighbour_epoch(p.epoch, true, &next)) return TT_END;
        int rc = cross_into(next);
        if (rc != TT_OK) return rc;
        tt_position(&p);
    }
    int rc = advance_to(p.epoch, p.step, machine_at_entry() ? 1 : p.insn + 1);
    if (rc == TT_SEGMENT_ENDED) return TT_OK;   /* landed on the next entry */
    return rc;
}

int tt_reverse_stepi(void) {
    tt_pos_t p;
    tt_position(&p);
    if (!machine_at_entry()) {
        return tt_goto(p.epoch, p.step, p.insn > 1 ? p.insn - 1 : 0);
    }
    uint64_t epoch = p.epoch, step = p.step;
    if (step == 0) {
        /* (prev, len) is this keyframe: the moment before it is the last
         * position of the previous non-empty epoch. */
        uint64_t prev = epoch, len = 0;
        do {
            if (!neighbour_epoch(prev, false, &prev)) return TT_BEGIN;
            if (!tt_epoch_len(prev, &len)) return TT_ERR_JOURNAL;
        } while (len == 0);
        epoch = prev;
        step = len;
    }
    uint64_t n;
    int rc = tt_segment_length(epoch, step - 1, &n);
    if (rc != TT_OK) return rc;
    return tt_goto(epoch, step - 1, n + 1);
}

/* ---- Breakpoint and watchpoint searches ---- */

typedef struct {
    bool     found;
    uint64_t segment;
    uint32_t ordinal;
    uint64_t insn;
    uint64_t dr6;
    uint64_t insn_limit;   /* partial segment: only hits before this position */
} hit_log_t;

static void log_hit(void* c, uint64_t segment, uint32_t ordinal, uint64_t insn, uint64_t dr6) {
    hit_log_t* h = (hit_log_t*)c;
    if (h->insn_limit && insn >= h->insn_limit) return;
    h->found = true;
    h->segment = segment;
    h->ordinal = ordinal;
    h->insn = insn;
    h->dr6 = dr6;
}

static void report_hit(tt_hit_t* hit, uint64_t dr6) {
    if (!hit) return;
    hit->hit = true;
    hit->dr6 = dr6;
    hit->slot = 0;
    for (uint32_t i = 0; i < 4; i++) {
        if (dr6 & (1u << i)) { hit->slot = i; break; }
    }
}

/* Land exactly on the `ordinal`-th hit of the segment after entry `segment`,
 * single-stepping it so the instruction position is exact. */
static int locate_hit(uint64_t epoch, uint64_t segment, uint32_t ordinal, tt_hit_t* hit) {
    int rc = tt_goto(epoch, segment, 0);
    if (rc != TT_OK) return rc;
    machine_set_hwbp_active(true);
    machine_set_count_insns(true);
    machine_set_target(segment + 1, 0, segment + 1);   /* never past the segment */
    for (;;) {
        machine_resume();
        const machine_stop_t* st = machine_last_stop();
        if (st->reason != STOP_BREAKPOINT) {
            rc = TT_ERR_REPLAY;
            break;
        }
        if (st->code >= ordinal) {
            report_hit(hit, st->dr6);
            rc = TT_OK;
            break;
        }
    }
    machine_clear_target();
    machine_set_count_insns(false);
    machine_set_hwbp_active(false);
    kreverse_set_position(machine_epoch(), machine_step());
    return rc;
}

int tt_continue(tt_hit_t* hit) {
    if (hit) hit->hit = false;
    if (machine_mode() != MACHINE_REPLAY || tt_at_end()) return TT_END;
    if (!machine_hwbps_set()) {
        tt_pos_t e = { g_end_epoch, g_end_step, 0 };
        int rc = tt_goto_pos(&e);
        return rc == TT_OK ? TT_END : rc;
    }
    for (;;) {
        tt_pos_t p;
        tt_position(&p);
        uint64_t len;
        if (!tt_epoch_len(p.epoch, &len)) return TT_ERR_JOURNAL;
        if (machine_at_entry() && p.step == len) {
            if (tt_at_end()) return TT_END;
            uint64_t next;
            if (!neighbour_epoch(p.epoch, true, &next)) return TT_END;
            int rc = cross_into(next);
            if (rc != TT_OK) return rc;
            continue;
        }
        uint64_t from = machine_step();
        machine_set_target(len, 0, len);
        machine_set_hwbp_active(true);
        machine_resume();
        machine_set_hwbp_active(false);
        machine_clear_target();
        g_stats.replayed_steps += machine_step() - from;
        const machine_stop_t* st = machine_last_stop();
        if (st->reason == STOP_BREAKPOINT) {
            /* Stopped right at the hit; re-run its segment to learn the
             * exact instruction position. */
            return locate_hit(p.epoch, machine_step(), (uint32_t)st->code, hit);
        }
        if (st->reason != STOP_TARGET) return TT_ERR_REPLAY;
        kreverse_set_position(machine_epoch(), machine_step());
    }
}

/* Replay `epoch` from its keyframe up to entry `limit`, logging the last
 * hardware hit. */
static int scan_hits(uint64_t epoch, uint64_t limit, hit_log_t* log) {
    int rc = tt_goto(epoch, 0, 0);
    if (rc != TT_OK) return rc;
    if (limit == 0) return TT_OK;
    uint64_t len;
    tt_epoch_len(epoch, &len);
    machine_set_hit_hook(log_hit, log);
    machine_set_hwbp_active(true);
    rc = tt_drive(epoch, limit, 0, len);
    machine_set_hwbp_active(false);
    machine_set_hit_hook(0, 0);
    return rc;
}

int tt_reverse_continue(tt_hit_t* hit) {
    if (hit) hit->hit = false;
    tt_pos_t p;
    tt_position(&p);
    tt_window_t w;
    if (!tt_window(&w)) return TT_ERR_STATE;
    if (!machine_hwbps_set()) {
        /* Nothing to stop at: run back to the start of the recording. */
        int rc = tt_goto(w.oldest, 0, 0);
        return rc == TT_OK ? TT_BEGIN : rc;
    }

    /* 1. The partial segment before the current position. */
    if (!machine_at_entry() && p.insn >= 2) {
        hit_log_t log = { 0 };
        log.insn_limit = p.insn;
        int rc = tt_goto(p.epoch, p.step, 0);
        if (rc != TT_OK) return rc;
        machine_set_count_insns(true);
        machine_set_hit_hook(log_hit, &log);
        machine_set_hwbp_active(true);
        rc = advance_to(p.epoch, p.step, p.insn);
        machine_set_hwbp_active(false);
        machine_set_hit_hook(0, 0);
        machine_set_count_insns(false);
        if (rc != TT_OK) return rc;
        if (log.found) {
            rc = tt_goto(p.epoch, p.step, log.insn);
            report_hit(hit, log.dr6);
            return rc;
        }
    }

    /* 2. Whole segments before it, epoch by epoch, newest first. */
    uint64_t epoch = p.epoch, limit = p.step;
    for (;;) {
        hit_log_t log = { 0 };
        int rc = scan_hits(epoch, limit, &log);
        if (rc != TT_OK) return rc;
        if (log.found) return locate_hit(epoch, log.segment, log.ordinal, hit);
        if (!neighbour_epoch(epoch, false, &epoch)) break;
        if (!tt_epoch_len(epoch, &limit)) return TT_ERR_JOURNAL;
    }
    int rc = tt_goto(w.oldest, 0, 0);
    return rc == TT_OK ? TT_BEGIN : rc;
}

/* ---- Last change to a range of user memory (MCP watch_last_write) ---- */

typedef struct {
    uint32_t pid;
    uint64_t addr;
    uint32_t len;
} watch_t;

static uint64_t watch_probe(void* c) {
    watch_t* w = (watch_t*)c;
    process_t* p = proc_by_pid(w->pid);
    uint64_t v = 0;
    if (!p || !uaccess_read(p->address_space, w->addr, &v, w->len)) return ~0ULL;
    return v;
}

int tt_last_change(uint32_t pid, uint64_t addr, uint32_t len, tt_pos_t* where) {
    if (len == 0 || len > 8) return TT_ERR_RANGE;
    watch_t w = { pid, addr, len };
    int rc;
    if (!machine_at_entry()) {
        /* Inside a segment: the latest change may be in the part of it that
         * already ran. Walk it forward from its entry. */
        tt_pos_t p;
        tt_position(&p);
        if ((rc = tt_goto(p.epoch, p.step, 0)) != TT_OK) return rc;
        uint64_t v = watch_probe(&w), last = 0;
        for (uint64_t i = 1; i <= p.insn; i++) {
            if ((rc = advance_to(p.epoch, p.step, i)) != TT_OK) return rc;
            uint64_t now = watch_probe(&w);
            if (now != v) last = i;
            v = now;
        }
        if (last) {
            if ((rc = tt_goto(p.epoch, p.step, last)) != TT_OK) return rc;
            tt_position(where);
            return TT_OK;
        }
        /* Unchanged since entry p.step: search the whole steps before it. */
        if ((rc = tt_goto(p.epoch, p.step, 0)) != TT_OK) return rc;
    }
    kreverse_set_position(machine_epoch(), machine_step());
    reverse_pos_t hit;
    rc = krevbreak_watchpoint(watch_probe, &w, &hit);
    if (rc == REVBREAK_NOT_FOUND) return TT_BEGIN;
    if (rc != REVBREAK_OK) return TT_ERR_REPLAY;

    /* The value first differs at entry hit.offset; it was written in the
     * segment before it (while the previous entry was handled, or by an
     * instruction after it). Refine to the exact position. */
    uint64_t epoch = hit.epoch, seg;
    if (hit.offset > 0) {
        seg = hit.offset - 1;
    } else {
        uint64_t prev = epoch, plen = 0;
        do {
            if (!neighbour_epoch(prev, false, &prev) || !tt_epoch_len(prev, &plen)) {
                tt_position(where);
                return TT_OK;   /* cannot see before the oldest keyframe */
            }
        } while (plen == 0);
        epoch = prev;
        seg = plen - 1;
    }
    rc = tt_goto(epoch, seg, 0);
    if (rc != TT_OK) return rc;
    uint64_t old = watch_probe(&w);
    for (uint64_t i = 1;; i++) {
        rc = advance_to(epoch, seg, i);
        if (rc == TT_SEGMENT_ENDED) break;   /* changed by the next entry itself */
        if (rc != TT_OK) return rc;
        if (watch_probe(&w) != old) break;
    }
    tt_position(where);
    kreverse_set_position(machine_epoch(), machine_step());
    return TT_OK;
}
