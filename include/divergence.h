/* IKOS Orthogonal Persistence - Divergence Detector (#166, epic #159)
 *
 * The safety net for deterministic replay. At each epoch boundary the system
 * state is split into components (process table, scheduler, user pages, ...)
 * and each is checksummed. On a live run the checksums are recorded; on replay
 * they are recomputed and compared. Any mismatch is a nondeterminism leak that
 * the catalog (#160) and the record/replay wrappers (#162-#164) missed, and it
 * is reported with the epoch and the component that diverged. Kept on in debug
 * builds, it keeps the whole feature honest.
 *
 * The core is pure and host-testable: components are checksummed by the caller
 * (a helper crc32 is provided) and fed in by id. The kernel adapter
 * (divergence_sync.c) supplies the concrete component ids and a global detector
 * the replay driver drives at each epoch boundary.
 *
 * See docs/architecture/time-travel.md.
 */

#ifndef DIVERGENCE_H
#define DIVERGENCE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    DIVERGE_OFF = 0,   /* detector disabled: checks always pass, records nothing */
    DIVERGE_RECORD,    /* store each component's checksum per epoch */
    DIVERGE_REPLAY     /* compare recomputed checksums against the recorded ones */
} diverge_mode_t;

#define DIVERGE_OK              0
#define DIVERGE_ERR_PARAM      -1
#define DIVERGE_MAX_COMPONENTS 16

typedef struct {
    diverge_mode_t mode;
    uint64_t epoch;
    uint32_t sums[DIVERGE_MAX_COMPONENTS];     /* recorded (RECORD) / expected (REPLAY) */
    bool     present[DIVERGE_MAX_COMPONENTS];  /* component had a value this epoch */

    /* First divergence seen this session (sticky, so the first leak is kept). */
    bool     diverged;
    uint64_t diverged_epoch;
    uint32_t diverged_component;
    uint32_t diverged_expected;
    uint32_t diverged_actual;

    /* Stats over the session. */
    uint32_t checks;
    uint32_t mismatches;
} divergence_t;

/* Bind a detector and set its mode. */
int  divergence_init(divergence_t* d, diverge_mode_t mode);
void divergence_set_mode(divergence_t* d, diverge_mode_t mode);

/* Start an epoch boundary. Clears the per-epoch component values (but not the
 * sticky divergence verdict). */
void divergence_begin_epoch(divergence_t* d, uint64_t epoch);

/* RECORD: store a component's checksum for the current epoch. */
int  divergence_record(divergence_t* d, uint32_t component, uint32_t checksum);

/* Read back a recorded component checksum (RECORD), for persisting into the
 * journal. *present reports whether the component was recorded this epoch. */
uint32_t divergence_recorded(const divergence_t* d, uint32_t component, bool* present);

/* REPLAY: install the expected per-component checksums recorded for `epoch`.
 * sums[i] is component i's checksum for i in [0, n). */
int  divergence_expect(divergence_t* d, uint64_t epoch,
                       const uint32_t* sums, uint32_t n);

/* REPLAY: compare a component's recomputed checksum against the expected value.
 * Returns true on match. On mismatch (or a component with no expected value)
 * records the first divergence and returns false. OFF always returns true. */
bool divergence_check(divergence_t* d, uint32_t component, uint32_t checksum);

/* Whether any divergence has been detected this session. */
bool divergence_has_diverged(const divergence_t* d);

/* CRC32 (IEEE 802.3, reflected, poly 0xEDB88320), seed 0. A convenient way to
 * checksum a component's state. Exposed for callers and tests. */
uint32_t divergence_checksum(uint32_t seed, const void* data, uint32_t len);

/* ---- Kernel adapter (divergence_sync.c) ----
 * A global detector and the component ids the replay driver checksums at each
 * epoch boundary. Default mode is OFF. */
enum {
    KDIVERGE_PROCTABLE = 0,
    KDIVERGE_SCHEDULER,
    KDIVERGE_USER_PAGES,
    KDIVERGE_FILETABLE,
    KDIVERGE_IPC,
    KDIVERGE_CONTEXTS,          /* saved user registers of every process (#225) */
    KDIVERGE_COMPONENT_COUNT
};

void     kdiverge_init(void);
void     kdiverge_set_mode(diverge_mode_t mode);
void     kdiverge_begin_epoch(uint64_t epoch);
int      kdiverge_record(uint32_t component, uint32_t checksum);
int      kdiverge_expect(uint64_t epoch, const uint32_t* sums, uint32_t n);
bool     kdiverge_check(uint32_t component, uint32_t checksum);
bool     kdiverge_ok(void);   /* false once a divergence has been detected */
/* Clear the sticky verdict and stats before a fresh verification run (#225). */
void     kdiverge_reset(void);
/* The global detector, to report which epoch and component diverged. */
const divergence_t* kdiverge_detector(void);

#endif /* DIVERGENCE_H */
