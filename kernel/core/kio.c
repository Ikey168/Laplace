/* Laplace core: the machine's inputs, journaled (#242). See include/core/kio.h. */

#include "core/kio.h"
#include "core/kjournal.h"
#include "core/kfpages.h"
#include "core/machine.h"
#include "core/mm.h"
#include "core/console.h"
#include "iorec.h"
#include "journal_capture.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

static iorec_log_t    g_log;
static uint64_t       g_epoch;
static uint32_t       g_owned;        /* stream pages already handed to their keyframe */
static kio_mismatch_t g_mis;
static bool           g_perturb, g_perturb_on, g_perturb_applied;
static bool           g_perturb_rec, g_perturb_rec_on, g_perturb_rec_applied;
static kio_stats_t    g_stats;
static uint32_t       g_sabotage;

void kio_sabotage(uint32_t kind) { g_sabotage = kind; }

const kio_stats_t* kio_stats(void) { return &g_stats; }
void kio_perturb_record_next(bool on) {
    g_perturb_rec = on;
    if (on) g_perturb_rec_applied = false;
}
bool kio_perturb_record_applied(void) { return g_perturb_rec_applied; }

static uint32_t page_alloc(void* c) { (void)c; return kfp_payload_alloc(); }
static int page_write(void* c, uint32_t id, const uint8_t* p) { (void)c; return kfp_payload_write(id, p); }
static int page_read(void* c, uint32_t id, uint8_t* p) { (void)c; return kfp_payload_read(id, p); }
static void* mem_alloc(uint64_t n) { return kmalloc(n); }
static void mem_free(void* p) { kfree(p); }

void kio_init(void) {
    static const iorec_env_t env = { page_alloc, page_write, page_read, mem_alloc, mem_free, 0 };
    iorec_init(&g_log, &env);
}

static bool recording(void) { return machine_mode() == MACHINE_LIVE; }

/* Move new records (and written pages) into the epoch's extended journal. */
static void journal_new(void) {
    iorec_event_t ev[16];
    uint32_t n;
    while ((n = iorec_take_events(&g_log, ev, 16)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (!kj_append(ev[i].type, ev[i].lclock, ev[i].value, ev[i].len)) {
                panic("kio: the epoch's journal is full");
            }
        }
    }
}

static void append(uint32_t kind, uint32_t nr, uint64_t args, int64_t ret, const void* p, uint32_t len) {
    if (machine_norecord()) return;
    int rc = iorec_append(&g_log, machine_step(), kind, nr, args, ret, p, len);
    if (rc != IOREC_OK) panic("kio: cannot record an input (%d); the page pool may be full", rc);
    g_stats.records++;
    g_stats.bytes += len;
    journal_new();
}

/* Replay: the next record, or stop the replay here. */
static void next(uint32_t kind, uint32_t nr, uint64_t args, iorec_t* r) {
    int rc = iorec_next(&g_log, machine_step(), kind, nr, args, r);
    if (rc == IOREC_OK) return;
    g_mis.valid = true;
    g_mis.epoch = g_epoch;
    g_mis.step = machine_step();
    g_mis.kind = kind;
    g_mis.nr = nr;
    g_mis.exhausted = rc == IOREC_EXHAUSTED;
    g_mis.rec_kind = rc == IOREC_MISMATCH ? r->kind : 0;
    g_mis.rec_nr = rc == IOREC_MISMATCH ? r->nr : 0;
    g_mis.rec_step = rc == IOREC_MISMATCH ? r->step : 0;
    machine_stop_diverged();
}

uint64_t kio_time(uint64_t (*live)(void)) {
    if (recording()) {
        uint64_t v = live();
        append(IOREC_TIME, 0, 0, (int64_t)v, 0, 0);
        return v;
    }
    iorec_t r;
    next(IOREC_TIME, 0, 0, &r);
    return (uint64_t)r.ret + (g_perturb_on ? 1000000 : 0) + (g_sabotage == KIO_SABOTAGE_CLOCK ? 1000000 : 0);
}

int kio_entropy(void* buf, uint32_t len, int (*live)(void* ctx, void* buf, uint32_t len), void* ctx) {
    if (recording()) {
        int rc = live(ctx, buf, len);
        if (rc != 0) return rc;
        append(IOREC_ENTROPY, 0, len, 0, buf, len);
        return 0;
    }
    iorec_t r;
    next(IOREC_ENTROPY, 0, len, &r);
    if (iorec_read(&g_log, r.off, buf, len) != IOREC_OK) panic("kio: cannot read a recorded input");
    if (g_perturb_on) {
        for (uint32_t i = 0; i < len; i++) ((uint8_t*)buf)[i] ^= 0x5A;
    }
    if (g_sabotage == KIO_SABOTAGE_ENTROPY && len) ((uint8_t*)buf)[0] ^= 1;
    return 0;
}

int64_t kio_input(uint32_t kind, uint32_t nr, uint64_t args,
                  int64_t (*live)(void* ctx, void* buf, uint32_t cap), void* ctx,
                  void* buf, uint32_t cap) {
    if (recording()) {
        int64_t ret = live(ctx, buf, cap);
        uint32_t plen = ret > 0 ? (uint32_t)(ret < (int64_t)cap ? ret : cap) : 0;
        append(kind, nr, args, ret, buf, plen);
        return ret;
    }
    iorec_t r;
    next(kind, nr, args, &r);
    if (r.len > cap || iorec_read(&g_log, r.off, buf, r.len) != IOREC_OK) {
        panic("kio: cannot read a recorded input");
    }
    return r.ret;
}

void kio_event(uint32_t kind, uint32_t nr, uint64_t args, uint64_t step) {
    if (machine_norecord()) return;
    int rc = iorec_append(&g_log, step, kind, nr, args, 0, 0, 0);
    if (rc != IOREC_OK) panic("kio: cannot record an event (%d)", rc);
    g_stats.records++;
    journal_new();
}

bool kio_event_next(uint32_t kind, uint64_t step, uint32_t* nr, uint64_t* args) {
    iorec_t r;
    if (iorec_peek(&g_log, &r) != IOREC_OK || r.kind != kind || r.step != step) return false;
    iorec_skip(&g_log);
    *nr = r.nr;
    *args = r.args;
    return true;
}

void kio_record(uint32_t kind, uint64_t step, uint64_t args, const void* payload, uint32_t len) {
    if (machine_norecord()) return;
    int rc = iorec_append(&g_log, step, kind, 0, args, 0, payload, len);
    if (rc != IOREC_OK) panic("kio: cannot record (%d); the page pool may be full", rc);
    g_stats.records++;
    g_stats.bytes += len;
    journal_new();
}

bool kio_record_peek(uint32_t kind, uint64_t step, uint64_t* args) {
    iorec_t r;
    if (iorec_peek(&g_log, &r) != IOREC_OK || r.kind != kind || r.step != step) return false;
    *args = r.args;
    return true;
}

bool kio_record_next(uint32_t kind, uint64_t step, uint64_t* args, void* buf, uint32_t cap,
                     uint32_t* len) {
    iorec_t r;
    if (iorec_peek(&g_log, &r) != IOREC_OK || r.kind != kind || r.step != step) return false;
    if (r.len > cap) {
        *len = r.len;                          /* the caller grows its buffer */
        return false;
    }
    if (iorec_read(&g_log, r.off, buf, r.len) != IOREC_OK) panic("kio: cannot read a recorded input");
    iorec_skip(&g_log);
    *args = r.args;
    *len = r.len;
    return true;
}

void kio_diverged(uint32_t kind, uint64_t rec_step) {
    kio_note_diverged(kind, rec_step);
    machine_stop_diverged();
}

void kio_note_diverged(uint32_t kind, uint64_t rec_step) {
    g_mis.valid = true;
    g_mis.epoch = g_epoch;
    g_mis.step = machine_step();
    g_mis.kind = kind;
    g_mis.nr = 0;
    g_mis.rec_kind = kind;
    g_mis.rec_nr = 0;
    g_mis.rec_step = rec_step;
    g_mis.exhausted = false;
}

/* ---- Epochs ---- */

void kio_begin_epoch(uint64_t epoch) {
    /* Pages written for inputs no journal kept (before the first keyframe,
     * or a timeline abandoned on resume) are free again. */
    const uint32_t* ids;
    uint32_t n = iorec_pages(&g_log, &ids);
    if (n > g_owned) kfp_payload_free(ids + g_owned, n - g_owned);
    g_epoch = epoch;
    iorec_reset(&g_log);
    g_owned = 0;
}

void kio_close_epoch(uint64_t epoch) {
    if (iorec_flush(&g_log) != IOREC_OK) panic("kio: cannot write the last input page");
    journal_new();
    const uint32_t* ids;
    uint32_t n = iorec_pages(&g_log, &ids);
    if (n > g_owned) kfp_payload_own(epoch, ids + g_owned, n - g_owned);
    g_owned = n;
}

void kio_replay_begin(uint64_t epoch) {
    g_epoch = epoch;
    iorec_reset(&g_log);
    g_owned = 0;
    g_perturb_on = false;
    if (g_perturb) {
        g_perturb = false;
        g_perturb_on = true;
    }
    g_perturb_rec_on = g_perturb_rec;
    g_perturb_rec = false;
}

void kio_replay_event(uint32_t type, uint32_t len, uint64_t lclock, uint64_t value) {
    if (type < IOREC_EV_HEAD || type > IOREC_EV_PAGE) return;
    if (iorec_load_event(&g_log, type, len, lclock, value) != IOREC_OK) {
        kprintf("kio: epoch %lu: a damaged input record\n", g_epoch);
    }
    if (type == IOREC_EV_BODY && g_perturb_rec_on && iorec_count(&g_log) == 1) {
        g_log.recs[0].args ^= 1;                  /* test hook */
        g_perturb_rec_on = false;
        g_perturb_rec_applied = true;
    }
    if (type == IOREC_EV_HEAD && g_perturb_on) {
        uint32_t kind = len >> 16;
        if (kind == IOREC_TIME || kind == IOREC_ENTROPY) g_perturb_applied = true;
    }
    const uint32_t* ids;
    g_owned = iorec_pages(&g_log, &ids);   /* loaded pages are their keyframe's already */
}

void kio_continue(void) {
    iorec_continue(&g_log);
    g_perturb_on = false;
}

uint64_t kio_epoch_bytes(void) { return iorec_bytes(&g_log); }

void kio_adopt_journal(uint64_t epoch) {
    journal_reader_t rd;
    if (journal_capture_load(epoch, &rd) != JOURNAL_OK) return;
    journal_event_t ev;
    while (journal_reader_next(&rd, &ev) == JOURNAL_OK) {
        if (ev.type != IOREC_EV_PAGE) continue;
        uint32_t id = (uint32_t)ev.value, count = (uint32_t)(ev.value >> 32);
        for (uint32_t k = 0; k < count; k++) {
            uint32_t one = id + k;
            kfp_payload_adopt(epoch, &one, 1);
        }
    }
}

const kio_mismatch_t* kio_mismatch(void) { return &g_mis; }
void kio_clear_mismatch(void) { memset(&g_mis, 0, sizeof(g_mis)); }

void kio_perturb_next(bool on) {
    g_perturb = on;
    if (on) g_perturb_applied = false;
}

bool kio_perturb_applied(void) { return g_perturb_applied; }
