/* Laplace core: the extended journal (#241). See include/core/kjournal.h. */

#include "core/kjournal.h"
#include "core/kio.h"

static journal_capture_event_t g_ev[KJ_MAX_EVENTS];
static uint32_t g_n;
static uint64_t g_epoch;
static uint32_t g_limit = 6144;

void kj_set_limit(uint32_t n) { g_limit = n < KJ_MAX_EVENTS ? n : KJ_MAX_EVENTS; }
uint32_t kj_limit(void) { return g_limit; }

void kj_begin_epoch(uint64_t epoch) {
    g_epoch = epoch;
    g_n = 0;
}

bool kj_append(uint32_t type, uint64_t lclock, uint64_t value, uint32_t len) {
    if (g_n >= g_limit) return false;
    journal_capture_event_t* e = &g_ev[g_n++];
    e->type = type;
    e->len = len;
    e->lclock = lclock;
    e->value = value;
    return true;
}

uint32_t kj_count(void) { return g_n; }

uint32_t kj_events(const journal_capture_event_t** out) {
    *out = g_ev;
    return g_n;
}

void kj_replay_begin(uint64_t epoch) {
    kj_begin_epoch(epoch);
    kio_replay_begin(epoch);
}

void kj_replay_add(uint32_t type, uint32_t len, uint64_t lclock, uint64_t value) {
    kj_append(type, lclock, value, len);
    kio_replay_event(type, len, lclock, value);   /* inputs (#242) */
}

bool kj_find(uint32_t type, uint64_t lclock, uint64_t* value) {
    for (uint32_t i = 0; i < g_n; i++) {
        if (g_ev[i].type == type && g_ev[i].lclock == lclock) {
            *value = g_ev[i].value;
            return true;
        }
    }
    return false;
}
