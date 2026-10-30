/* Laplace core: the extended journal (#241).
 *
 * The fixed journal streams (preemption, time, entropy) cover the inputs the
 * first programs could observe. Everything newer rides here: typed events
 * (type >= JOURNAL_EV_EXTENDED_BASE) appended in order while recording,
 * written into the epoch's journal after the entropy run, and loaded back
 * while replaying that epoch.
 *
 * One buffer serves both directions. Recording appends to it; loading an epoch
 * for replay fills it; switching a replay back to recording mid-epoch keeps
 * the loaded events as the prefix the continuation appends to.
 */

#ifndef CORE_KJOURNAL_H
#define CORE_KJOURNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "journal_capture.h"

/* Register trace (#241): every KJ_TRACE_EVERY entries, the rolling hash of the
 * entering process's registers. lclock = step, value = hash. A replay compares
 * its own hash at the same step, which bounds where a divergence began. */
#define KJ_EV_TRACE      (JOURNAL_EV_EXTENDED_BASE + 0)
#define KJ_TRACE_EVERY   16

#define KJ_MAX_EVENTS    32768

/* The events an epoch may hold: set from the journal slot size (#242). */
void     kj_set_limit(uint32_t n);
uint32_t kj_limit(void);

/* Recording: start epoch `epoch` with an empty buffer. */
void     kj_begin_epoch(uint64_t epoch);
/* Recording: append one event. False when the buffer is full. */
bool     kj_append(uint32_t type, uint64_t lclock, uint64_t value, uint32_t len);
uint32_t kj_count(void);
/* The buffer, for the journal capture source. */
uint32_t kj_events(const journal_capture_event_t** out);

/* Replay: load `epoch`'s events (called as its journal is read). */
void     kj_replay_begin(uint64_t epoch);
void     kj_replay_add(uint32_t type, uint32_t len, uint64_t lclock, uint64_t value);
/* The value of the event of `type` stamped `lclock`, if the epoch has one. */
bool     kj_find(uint32_t type, uint64_t lclock, uint64_t* value);

#endif /* CORE_KJOURNAL_H */
