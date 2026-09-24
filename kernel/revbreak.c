/* IKOS Orthogonal Persistence - Reverse Breakpoints and Watchpoints (#171)
 *
 * See include/revbreak.h and docs/architecture/time-travel.md.
 *
 * Pure layer over reverse execution (#170): no allocator, no hardware, no I/O
 * of its own. Both operations scan backward via reverse_step / reverse_continue,
 * which restore the nearest keyframe and replay to each candidate position, so
 * the injected predicate reads the real system state there.
 */

#include "revbreak.h"

typedef struct {
    revbreak_cond_fn cond;
    void*            ctx;
} bp_pack_t;

/* Adapt a breakpoint condition to the reverse-continue stop predicate. The
 * position is ignored: the condition reads the state restored at it. */
static bool bp_adapter(void* c, reverse_pos_t pos) {
    (void)pos;
    bp_pack_t* b = (bp_pack_t*)c;
    return b->cond(b->ctx);
}

int reverse_breakpoint(reverse_ctx_t* rv, revbreak_cond_fn cond, void* ctx,
                       reverse_pos_t* hit) {
    if (!rv || !cond) return REVBREAK_ERR_PARAM;
    bp_pack_t pack = { cond, ctx };
    int rc = reverse_continue(rv, bp_adapter, &pack);
    if (rc == REVERSE_OK) {
        if (hit) *hit = reverse_position(rv);
        return REVBREAK_OK;
    }
    if (rc == REVERSE_AT_START) return REVBREAK_NOT_FOUND;
    return REVBREAK_ERR;
}

/* ---- Scan fast path ---- */

/* The retained keyframe strictly before `epoch`, skipping empty epochs, with
 * its length. False when none is retained. */
static bool prev_epoch(reverse_ctx_t* rv, uint64_t epoch, uint64_t* prev, uint64_t* len) {
    while (epoch > 0) {
        uint64_t kf;
        if (!keyframe_ring_find(rv->rw->ring, epoch - 1, 0, &kf)) return false;
        uint64_t n = rv->epoch_len(rv->ctx, kf);
        if (n > 0) {
            *prev = kf;
            *len = n;
            return true;
        }
        epoch = kf;
    }
    return false;
}

static int land(reverse_ctx_t* rv, reverse_pos_t p, reverse_pos_t* hit) {
    if (rewind_to(rv->rw, p.epoch, p.offset) != REWIND_OK) return REVBREAK_ERR;
    reverse_set_position(rv, p.epoch, p.offset);
    if (hit) *hit = p;
    return REVBREAK_OK;
}

/* A miss leaves the system at the oldest retained moment, like the
 * step-by-step search (and like gdb's reverse-continue with no hit). */
static int miss(reverse_ctx_t* rv) {
    uint64_t oldest;
    if (keyframe_ring_oldest(rv->rw->ring, &oldest)) {
        reverse_pos_t o = { oldest, 0 };
        if (land(rv, o, 0) != REVBREAK_OK) return REVBREAK_ERR;
    }
    return REVBREAK_NOT_FOUND;
}

typedef struct {
    revbreak_cond_fn cond;
    void*            ctx;
    bool             found;
    reverse_pos_t    last;      /* last position where cond held */
} bp_scan_t;

static void bp_visit(void* v, reverse_pos_t pos) {
    bp_scan_t* s = (bp_scan_t*)v;
    if (s->cond(s->ctx)) {
        s->found = true;
        s->last = pos;
    }
}

int reverse_breakpoint_scan(reverse_ctx_t* rv, revbreak_scan_fn scan, void* sctx,
                            revbreak_cond_fn cond, void* ctx, reverse_pos_t* hit) {
    if (!rv || !rv->rw || !scan || !cond) return REVBREAK_ERR_PARAM;
    const reverse_pos_t cur = reverse_position(rv);
    uint64_t epoch = cur.epoch;
    uint64_t limit = cur.offset;   /* strictly before the current point */
    for (;;) {
        if (limit > 0) {
            bp_scan_t s = { cond, ctx, false, { 0, 0 } };
            if (scan(sctx, epoch, limit, bp_visit, &s) != 0) return REVBREAK_ERR;
            if (s.found) return land(rv, s.last, hit);
        }
        if (!prev_epoch(rv, epoch, &epoch, &limit)) return miss(rv);
    }
}

typedef struct {
    revbreak_probe_fn probe;
    void*             ctx;
    bool              any;          /* visited at least one position */
    uint64_t          first_val;    /* value at the epoch's first visited position */
    uint64_t          prev_val;     /* value at the latest visited position */
    bool              changed;
    reverse_pos_t     last_change;  /* first position holding the latest new value */
} wp_scan_t;

static void wp_visit(void* v, reverse_pos_t pos) {
    wp_scan_t* s = (wp_scan_t*)v;
    uint64_t val = s->probe(s->ctx);
    if (!s->any) {
        s->any = true;
        s->first_val = val;
    } else if (val != s->prev_val) {
        s->changed = true;
        s->last_change = pos;
    }
    s->prev_val = val;
}

int reverse_watchpoint_scan(reverse_ctx_t* rv, revbreak_scan_fn scan, void* sctx,
                            revbreak_probe_fn probe, void* ctx, reverse_pos_t* hit) {
    if (!rv || !rv->rw || !scan || !probe) return REVBREAK_ERR_PARAM;
    const reverse_pos_t cur = reverse_position(rv);
    uint64_t epoch = cur.epoch;
    uint64_t limit = cur.offset + 1;   /* at or before the current point */
    bool     have_newer = false;       /* a newer (later) region was scanned */
    uint64_t newer_first_val = 0;
    reverse_pos_t newer_first = { 0, 0 };

    for (;;) {
        wp_scan_t s = { probe, ctx, false, 0, 0, false, { 0, 0 } };
        if (scan(sctx, epoch, limit, wp_visit, &s) != 0) return REVBREAK_ERR;
        if (s.any) {
            /* A change across the boundary into the newer region is more
             * recent than any change inside this epoch. */
            if (have_newer && s.prev_val != newer_first_val) return land(rv, newer_first, hit);
            if (s.changed) return land(rv, s.last_change, hit);
            have_newer = true;
            newer_first_val = s.first_val;
            newer_first.epoch = epoch;
            newer_first.offset = 0;
        }
        if (!prev_epoch(rv, epoch, &epoch, &limit)) return miss(rv);
    }
}

int reverse_watchpoint(reverse_ctx_t* rv, revbreak_probe_fn probe, void* ctx,
                       reverse_pos_t* hit) {
    if (!rv || !rv->rw || !probe) return REVBREAK_ERR_PARAM;

    /* Land at the current position so the probe reads its value. */
    reverse_pos_t cur = reverse_position(rv);
    if (rewind_to(rv->rw, cur.epoch, cur.offset) != REWIND_OK) return REVBREAK_ERR;
    uint64_t newer = probe(ctx);

    /* Walk backward, comparing each position's value to the one after it. The
     * first difference (going backward) is the most recent write. */
    for (;;) {
        reverse_pos_t before = reverse_position(rv);  /* position holding `newer` */
        int rc = reverse_step(rv);
        if (rc == REVERSE_AT_START) return REVBREAK_NOT_FOUND;
        if (rc != REVERSE_OK) return REVBREAK_ERR;

        uint64_t older = probe(ctx);                  /* value one step earlier */
        if (older != newer) {
            /* The value changed between here and `before`: the write is at
             * `before`. Land there. */
            if (rewind_to(rv->rw, before.epoch, before.offset) != REWIND_OK)
                return REVBREAK_ERR;
            reverse_set_position(rv, before.epoch, before.offset);
            if (hit) *hit = before;
            return REVBREAK_OK;
        }
        newer = older;
    }
}
