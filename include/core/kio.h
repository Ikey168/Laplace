/* Laplace core: the machine's inputs, journaled (#242).
 *
 * One generic record (include/iorec.h) for everything that enters the
 * machine from outside: clock reads (ktime_read), entropy (kentropy_fill),
 * and system calls answered from outside (kio_input, for console and network
 * input). While recording, the live source runs and the result is appended
 * to the epoch's log, its payload to a byte stream whose pages go to the
 * keyframe page pool (#244). While replaying, the source never runs: the
 * call must be the one recorded at this step (same kind, call number, and
 * argument digest), and gets the recorded result and payload. A call that
 * does not match stops the replay there: verify reports the epoch as
 * diverged in the "inputs" component, at that step.
 *
 * Records ride in the epoch's journal as extended events; payload pages are
 * owned by the epoch's keyframe and freed with it.
 */

#ifndef CORE_KIO_H
#define CORE_KIO_H

#include <stdint.h>
#include <stdbool.h>

#define KDIVERGE_INPUTS 15        /* divergence component: a replayed input */

void     kio_init(void);

/* Recording or replaying, by the machine's mode. */
uint64_t kio_time(uint64_t (*live)(void));
int      kio_entropy(void* buf, uint32_t len, int (*live)(void* ctx, void* buf, uint32_t len),
                     void* ctx);
/* A system call answered from outside: live(ctx, buf, cap) produces the
 * result and up to `cap` payload bytes (when the result is >= 0, it is the
 * payload length). */
int64_t  kio_input(uint32_t kind, uint32_t nr, uint64_t args,
                   int64_t (*live)(void* ctx, void* buf, uint32_t cap), void* ctx,
                   void* buf, uint32_t cap);

/* Events delivered to the machine (a timer expiry waking a process, device
 * input arriving) rather than asked for by a call (#246): recording appends
 * one at `step`; replaying takes the next record if it is one of `kind` at
 * `step`. */
void     kio_event(uint32_t kind, uint32_t nr, uint64_t args, uint64_t step);
bool     kio_event_next(uint32_t kind, uint64_t step, uint32_t* nr, uint64_t* args);

/* Records the machine makes itself, with a payload (#245's preemption
 * points): recording appends one at `step`, args a digest of the payload;
 * replaying takes the next record if it is one of `kind` at `step`, copying
 * up to `cap` payload bytes. */
void     kio_record(uint32_t kind, uint64_t step, uint64_t args, const void* payload, uint32_t len);
bool     kio_record_next(uint32_t kind, uint64_t step, uint64_t* args, void* buf, uint32_t cap,
                         uint32_t* len);
/* (When the payload is larger than `cap`, false with *len set to its size,
 * and the record stays next.) */
/* Replay: the machine did not do what the record of `kind` at `rec_step`
 * says (a preemption point never reached, a record that does not decode):
 * stop the replay here, as an input mismatch. Does not return. */
void     kio_diverged(uint32_t kind, uint64_t rec_step);
/* The same, only noted: for a caller that stops the machine itself. */
void     kio_note_diverged(uint32_t kind, uint64_t rec_step);

/* Epochs. */
void     kio_begin_epoch(uint64_t epoch);   /* recording */
void     kio_close_epoch(uint64_t epoch);   /* recording: before the journal is written */
void     kio_replay_begin(uint64_t epoch);
void     kio_replay_event(uint32_t type, uint32_t len, uint64_t lclock, uint64_t value);
void     kio_continue(void);                /* a replayed epoch continues as a recording */
uint64_t kio_epoch_bytes(void);

/* Boot: the payload pages a retained epoch's journal names. */
void     kio_adopt_journal(uint64_t epoch);

/* The last replay mismatch, if the replay stopped on one. */
typedef struct {
    bool     valid;
    uint64_t epoch, step;
    uint32_t kind, nr;            /* the call the replay made */
    uint32_t rec_kind, rec_nr;    /* the one recorded next */
    uint64_t rec_step;
    bool     exhausted;           /* nothing was recorded */
} kio_mismatch_t;
const kio_mismatch_t* kio_mismatch(void);
void     kio_clear_mismatch(void);

/* Test hook: the next loaded epoch's clock values and random bytes change. */
void     kio_perturb_next(bool on);
bool     kio_perturb_applied(void);
/* Test hook: the next loaded epoch's first record names other arguments, so
 * the replay's first input call no longer matches it. */
void     kio_perturb_record_next(bool on);
bool     kio_perturb_record_applied(void);   /* the epoch had a record to change */

/* Test hook (#256): every replay from now on gets its inputs wrong. */
#define KIO_SABOTAGE_CLOCK   1    /* each clock read one tick (1 ms) late */
#define KIO_SABOTAGE_ENTROPY 2    /* one bit of each random payload flipped */
void     kio_sabotage(uint32_t kind);

/* Inputs recorded since boot. */
typedef struct {
    uint64_t records;
    uint64_t bytes;
} kio_stats_t;
const kio_stats_t* kio_stats(void);

#endif /* CORE_KIO_H */
