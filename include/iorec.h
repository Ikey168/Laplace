/* Laplace: the generic input journal (#242).
 *
 * Everything that enters the machine from outside (clock reads, entropy, and
 * later console bytes and network packets) is one kind of record:
 *
 *   (step, kind, call number, argument digest, return value, payload)
 *
 * While recording, the live source runs and its result is appended. While
 * replaying, the source does not run: the next record must have the same
 * step, kind, call number, and argument digest (else the replay has
 * diverged, and the caller is told so at that step, the input's first
 * visible effect), and its return value and payload are handed back instead.
 *
 * Payload bytes go into one byte stream per epoch, split into pages that a
 * store callback persists as they fill (the keyframe page pool, #244), so a
 * large input costs pages, not journal events, and splits across pages
 * rather than failing. A record is two journal events, and each run of
 * stream pages with consecutive ids one more:
 *
 *   IOREC_EV_HEAD  lclock = step, value = return value, len = kind << 16 | nr
 *   IOREC_EV_BODY  lclock = argument digest, value = payload offset,
 *                  len = payload length
 *   IOREC_EV_PAGE  lclock = first page index, value = first id | count << 32
 *
 * Pure: the store and the record array's memory come from callbacks.
 * Host-tested (tests/test_iorec.c).
 */

#ifndef IOREC_H
#define IOREC_H

#include <stdint.h>
#include <stdbool.h>

/* Extended journal event types (JOURNAL_EV_EXTENDED_BASE is 16; kjournal.h
 * uses 16 for the register trace). */
#define IOREC_EV_HEAD  17
#define IOREC_EV_BODY  18
#define IOREC_EV_PAGE  19

/* One journal event, laid out as journal_capture_event_t. */
typedef struct {
    uint32_t type;
    uint32_t len;
    uint64_t lclock;
    uint64_t value;
} iorec_event_t;

#define IOREC_PAGE     4096u
#define IOREC_NONE     0xFFFFFFFFu

#define IOREC_OK         0
#define IOREC_ERR_FULL  -1   /* no page for the payload, or no memory */
#define IOREC_ERR_IO    -2
#define IOREC_MISMATCH  -3   /* replay: the call is not the recorded one */
#define IOREC_EXHAUSTED -4   /* replay: no record left */
#define IOREC_CORRUPT   -5

/* Kinds. */
#define IOREC_TIME     1     /* a clock read: ret = the value */
#define IOREC_ENTROPY  2     /* random bytes: the payload */
#define IOREC_SYSCALL  3     /* a system call answered from outside */
#define IOREC_DEVICE   4     /* device input delivered to the machine */

typedef struct {
    uint64_t step;
    uint32_t kind;
    uint32_t nr;
    uint64_t args;      /* digest of the arguments that matter */
    int64_t  ret;
    uint64_t off;       /* payload in the epoch's stream */
    uint32_t len;
} iorec_t;

typedef struct {
    uint32_t (*page_alloc)(void* ctx);                           /* IOREC_NONE when full */
    int      (*page_write)(void* ctx, uint32_t id, const uint8_t* page);
    int      (*page_read)(void* ctx, uint32_t id, uint8_t* page);
    void*    (*mem_alloc)(uint64_t bytes);
    void     (*mem_free)(void* p);
    void*    ctx;
} iorec_env_t;

typedef struct {
    iorec_env_t env;
    iorec_t*  recs;
    uint32_t  nrecs, cap;
    uint32_t* pages;            /* stream page ids, by index */
    uint32_t  npages, pcap;
    uint64_t  len;              /* stream bytes */
    uint8_t   tail[IOREC_PAGE]; /* the stream's last, partial page (recording) */
    uint32_t  cursor;           /* replay: next record */
    uint32_t  cache_id;         /* replay: the page in `cache` */
    uint8_t   cache[IOREC_PAGE];
    uint32_t  journaled_pages;  /* pages already given as events */
    uint32_t  journaled_recs;
} iorec_log_t;

void iorec_init(iorec_log_t* l, const iorec_env_t* env);
/* Forget everything (a new epoch, or before loading one). Pages already
 * written belong to their journal; they are not freed here. */
void iorec_reset(iorec_log_t* l);

/* Recording: append a record with its payload. */
int  iorec_append(iorec_log_t* l, uint64_t step, uint32_t kind, uint32_t nr, uint64_t args,
                  int64_t ret, const void* payload, uint32_t len);
/* Recording: write the partial last page, so the stream is all in pages. */
int  iorec_flush(iorec_log_t* l);
/* Events not yet journaled: records appended and pages written since the
 * last call (each record's two events, then each page's). */
uint32_t iorec_take_events(iorec_log_t* l, iorec_event_t* out, uint32_t max);

/* Replay: rebuild from an epoch's events (any order of the three types). */
int  iorec_load_event(iorec_log_t* l, uint32_t type, uint32_t len, uint64_t lclock, uint64_t value);
/* Replay: the next record, which must match. */
int  iorec_next(iorec_log_t* l, uint64_t step, uint32_t kind, uint32_t nr, uint64_t args,
                iorec_t* out);
/* Replay: the next record without taking it (IOREC_EXHAUSTED if none), and
 * taking it. For inputs that arrive rather than being asked for. */
int  iorec_peek(const iorec_log_t* l, iorec_t* out);
void iorec_skip(iorec_log_t* l);
/* Replay: payload bytes [off, off + len) of the stream. */
int  iorec_read(iorec_log_t* l, uint64_t off, void* buf, uint32_t len);
/* Replay -> recording: continue the loaded epoch. The stream resumes at the
 * next page boundary (its last page is already written). */
void iorec_continue(iorec_log_t* l);

/* Stream page ids, for whoever keeps the pages alive. */
uint32_t iorec_pages(const iorec_log_t* l, const uint32_t** ids);
uint64_t iorec_bytes(const iorec_log_t* l);
uint32_t iorec_count(const iorec_log_t* l);

#endif /* IOREC_H */
