/* Laplace core: recording, keyframes, restore, and replay in the booted
 * kernel (#221, #222, #223, #224, #225, #231).
 *
 * Recording. At the first kernel entry, and then whenever the checkpoint
 * interval elapses or a record buffer nears capacity, the kernel closes the
 * current epoch (its journal goes to the journal ring) and takes a keyframe at
 * that entry: checkpoint_take() marks every user page copy-on-write and
 * captures registers, and a kernel-state blob captures the process table,
 * regions, run order, and the pending entry. The keyframe's writeback runs at a
 * later entry, so processes write in between and the COW path preserves the
 * checkpoint-time images.
 *
 * Replay. tt_goto() restores a keyframe (tearing down the live processes and
 * rebuilding them from the store), loads that epoch's journal into the record
 * wrappers in REPLAY mode, and re-executes the user processes to the target.
 */

#ifndef CORE_TIMETRAVEL_H
#define CORE_TIMETRAVEL_H

#include <stdint.h>
#include <stdbool.h>
#include "fat.h"
#include "process.h"
#include "divergence.h"

/* ---- Setup ---- */

typedef struct {
    uint32_t interval_ticks;     /* keyframe cadence */
    uint32_t keyframes;          /* retained keyframes (rewind horizon) */
    uint32_t stop_after;         /* stop for the debugger after N keyframes (0: never) */
} tt_config_t;

/* Arm the stores on `dev` (formatting it if it holds none). Returns 0. Sets
 * *had_keyframe when the device already holds a retained keyframe. */
int  tt_init(fat_block_device_t* dev, const tt_config_t* cfg, bool* had_keyframe);
bool tt_ready(void);
/* Bind rewind / reverse / revbreak to the keyframe ring and the replay driver
 * (#226). Call after tt_init(). */
void tt_bind_verbs(void);
/* A RAM-backed block device for when no disk is attached (volatile). */
fat_block_device_t* tt_ram_device(uint32_t sectors);
uint32_t tt_required_sectors(uint32_t keyframes);

/* ---- Machine hooks (live recording) ---- */
void tt_on_tick(void);
void tt_on_live_entry(void);
void tt_before_process_exit(process_t* p);

/* ---- Stopping and resuming ---- */

/* At a live stop: finish any pending writeback and persist the open epoch's
 * journal up to the stop point, so the whole run is replayable. */
int tt_seal(void);

/* The recorded window: oldest retained keyframe, newest keyframe, and the step
 * count of the newest epoch (the stop point). */
typedef struct {
    uint64_t oldest;
    uint64_t newest;
    uint64_t end_step;
    uint32_t count;
} tt_window_t;
bool tt_window(tt_window_t* w);

/* Steps in epoch `epoch` (from its journal); false if unknown. */
bool tt_epoch_len(uint64_t epoch, uint64_t* len);

/* ---- Navigation ---- */

#define TT_OK            0
#define TT_ERR_RANGE    -1   /* outside the recorded window */
#define TT_ERR_RESTORE  -2
#define TT_ERR_JOURNAL  -3
#define TT_ERR_REPLAY   -4   /* replay stopped somewhere other than the target */
#define TT_ERR_STATE    -5

/* Reconstruct (epoch, step, insn) by restoring and re-executing. insn > 0 is
 * that many user instructions after entry `step` was handled (#228). */
int tt_goto(uint64_t epoch, uint64_t step, uint64_t insn);

/* Replay to the end of the recording and switch back to live recording. */
int tt_resume_live(void);

/* Number of user instructions between entry `step` (handled) and the next
 * entry of epoch `epoch` (#228). */
int tt_segment_length(uint64_t epoch, uint64_t step, uint64_t* insns);

/* Re-execute `epoch` from its keyframe and call visit(ctx, step) at every entry
 * 0 .. limit-1 with the machine there. */
typedef void (*tt_visit_fn)(void* ctx, uint64_t step);
int tt_scan_epoch(uint64_t epoch, uint64_t limit, tt_visit_fn visit, void* ctx);

/* ---- Verification (#225) ---- */

typedef struct {
    uint32_t epochs_checked;
    uint32_t epochs_diverged;
    bool     diverged;
    uint64_t first_epoch;        /* first diverging epoch */
    uint32_t first_component;    /* KDIVERGE_* */
    uint32_t expected;
    uint32_t actual;
} tt_verify_t;

/* Replay every retained epoch from its keyframe to its end and compare the
 * reconstructed state's checksums with the ones recorded there. Leaves the
 * machine at the end of the recording. */
int tt_verify(tt_verify_t* out);

/* Deliberately perturb the next replay (for testing the detector): flip one
 * entropy byte of the loaded journal. */
void tt_perturb_next_replay(bool on);

/* ---- Boot resume (#231) ---- */

/* Rebuild the machine from the newest retained keyframe and continue live from
 * there. Returns 0 when the machine was restored. */
int tt_resume_from_store(void);

/* Rebuild the machine from keyframe `epoch` (the machine is left stopped at
 * (epoch, 0)). */
int tt_restore_keyframe(uint64_t epoch);

/* Statistics for the console and the front ends. */
typedef struct {
    uint64_t keyframes_taken;
    uint64_t writebacks;
    uint64_t cow_captures;       /* pages preserved by the COW hook */
    uint64_t journals_written;
    uint64_t restores;
    uint64_t replayed_steps;
} tt_stats_t;
const tt_stats_t* tt_stats(void);

#endif /* CORE_TIMETRAVEL_H */
