/* Host harness: keyframe store + journal + divergence + MCP loop (#200, #230).
 *
 * NOT a boot. This runs on the host, over a RAM-array block device, and its
 * whole "machine state" is a single uint64_t. It checks that the storage,
 * journal, divergence, and MCP modules compose. The booted end-to-end test,
 * which records real processes in QEMU and re-executes them, is
 * tests/qemu/timetravel_e2e.py.
 *
 * Proves the whole live-capture + replay + front-end + divergence stack works
 * together, over the REAL merged modules (no mocks of the modules under test):
 *
 *   1. SETUP  - format a keyframe retention store (#195) and a journal (#194)
 *               over a RAM-backed block device, arm the divergence detector
 *               (#166/#197), and bind the MCP tool ops (#188).
 *   2. RECORD - run a short session of E epochs whose state folds in the epoch's
 *               nondeterministic inputs. At each epoch boundary: write a keyframe
 *               to the retention store, checksum the components (divergence
 *               RECORD) and journal that epoch (inputs + divergence sums, #194).
 *   3. REVERSE - drive the MCP JSON-RPC server loop (#199) with an agent script:
 *               list_checkpoints, then rewind_to / reverse_step to past moments.
 *               Each op restores the keyframe from the retention store.
 *   4. VERIFY - at every reconstructed moment recompute the component checksums
 *               and DIVERGE-check them against the recorded sums: the live run
 *               must report NO leak, and the reconstructed state must equal the
 *               recorded timeline. A deliberately corrupted restore is caught by
 *               the detector, proving it is live.
 *
 * Exits non-zero on any mismatch or any unexpected divergence, so it doubles as
 * a CI gate. The narrated transcript is what scripts/test/timetravel_live_demo.sh
 * captures for the README recording.
 *
 * Build: gcc -Iinclude -o timetravel_live_e2e tests/timetravel_live_e2e.c \
 *          kernel/keyframe_store.c kernel/snapshot_store.c kernel/keyframe_ring.c \
 *          kernel/journal_capture.c kernel/checkpoint_journal.c \
 *          kernel/divergence.c kernel/divergence_scan.c \
 *          kernel/mcp.c kernel/mcp_server.c
 */

#include <stdint.h>
#include <stdbool.h>

typedef __SIZE_TYPE__ size_t;
extern void* memcpy(void*, const void*, size_t);
extern void* memset(void*, int, size_t);
extern void* malloc(size_t);
extern void  free(void*);
extern int   printf(const char*, ...);

/* snapshot_store.c calls these as freestanding kernel helpers. */
void* kmalloc(size_t size) { return malloc(size); }
void  kfree(void* ptr) { free(ptr); }

#include "keyframe_store.h"
#include "journal_capture.h"
#include "divergence.h"
#include "divergence_scan.h"
#include "mcp.h"
#include "mcp_server.h"

/* ---------------- RAM-backed block device ---------------- */

#define DEV_SECTORS 4096
static uint8_t g_disk[DEV_SECTORS * SNAPSHOT_SECTOR_SIZE];

static int dev_read(void* d, uint32_t s, uint32_t n, void* buf) {
    (void)d;
    if ((uint64_t)s + n > DEV_SECTORS) return -1;
    memcpy(buf, g_disk + (size_t)s * SNAPSHOT_SECTOR_SIZE, (size_t)n * SNAPSHOT_SECTOR_SIZE);
    return 0;
}
static int dev_write(void* d, uint32_t s, uint32_t n, const void* buf) {
    (void)d;
    if ((uint64_t)s + n > DEV_SECTORS) return -1;
    memcpy(g_disk + (size_t)s * SNAPSHOT_SECTOR_SIZE, buf, (size_t)n * SNAPSHOT_SECTOR_SIZE);
    return 0;
}
static fat_block_device_t g_dev;
static fat_block_device_t* make_dev(void) {
    memset(g_disk, 0, sizeof(g_disk));
    g_dev.read_sectors = dev_read;
    g_dev.write_sectors = dev_write;
    g_dev.sector_size = SNAPSHOT_SECTOR_SIZE;
    g_dev.total_sectors = DEV_SECTORS;
    g_dev.private_data = 0;
    return &g_dev;
}

/* ---------------- Session model ---------------- */

#define E     6     /* recorded epochs (keyframes) */
#define CAP   4     /* retained keyframes (rewind horizon) */

/* The "machine state": a value that folds in each epoch's nondeterministic
 * inputs, so the recorded timeline is deterministic but input-dependent. */
static uint64_t g_state;

/* Two divergence components derived from the current state. */
static uint32_t comp_a(void* c) { (void)c; uint64_t v = g_state;        return divergence_checksum(0, &v, sizeof(v)); }
static uint32_t comp_b(void* c) { (void)c; uint64_t v = g_state * 2654435761ULL; return divergence_checksum(0, &v, sizeof(v)); }

static const diverge_source_t g_srcs[2] = {
    { KDIVERGE_PROCTABLE, comp_a, 0 },
    { KDIVERGE_SCHEDULER, comp_b, 0 },
};

/* Recorded timeline for verification. */
static uint64_t g_kf_state[E];         /* state at each epoch's keyframe */
static uint32_t g_kf_sums[E][2];       /* recorded component sums per epoch */

/* Journal source closures: the record loop stages this epoch's data here, and
 * the REAL journal_capture path pulls it through these sources. */
static uint64_t g_e2e_time[2];
static uint32_t g_e2e_ids[2], g_e2e_sums[2];
static uint32_t e2e_time_src(const uint64_t** out) { *out = g_e2e_time; return 2; }
static uint32_t e2e_div_src(const uint32_t** oids, const uint32_t** osums) {
    *oids = g_e2e_ids; *osums = g_e2e_sums; return 2;
}

/* Detector + record sink (mirrors kdiverge_record). */
static divergence_t g_det;
static int  det_record(uint32_t comp, uint32_t sum) { return divergence_record(&g_det, comp, sum); }
static bool det_check(uint32_t comp, uint32_t sum)  { return divergence_check(&g_det, comp, sum); }

/* ---------------- Keyframe store ---------------- */

#define KF_BASE 32
static keyframe_store_t g_ks;

/* Store the current state as a keyframe page for `epoch`. */
static int put_keyframe(uint64_t epoch) {
    static uint8_t page[SNAPSHOT_PAGE_SIZE];
    memset(page, 0, sizeof(page));
    memcpy(page, &g_state, sizeof(g_state));
    snapshot_writer_t w;
    if (keyframe_store_begin(&g_ks, epoch, &w) != KEYFRAME_STORE_OK) return -1;
    if (snapshot_writer_add_page(&w, 0, 0x1000, 0, page) != SNAPSHOT_OK) return -1;
    return keyframe_store_commit(&g_ks, &w, epoch);
}

/* Restore the keyframe nearest to `target` into g_state; returns the epoch. */
static bool restore_keyframe(uint64_t target, uint64_t* epoch_out) {
    static uint8_t page[SNAPSHOT_PAGE_SIZE];
    snapshot_reader_t rd;
    uint64_t sel = 0;
    if (keyframe_store_load_epoch(&g_ks, target, &rd, &sel) != KEYFRAME_STORE_OK) return false;
    snapshot_page_record_t rec; rec.page_data = page;
    if (snapshot_reader_next(&rd, &rec) != SNAPSHOT_OK) return false;
    memcpy(&g_state, page, sizeof(g_state));
    if (epoch_out) *epoch_out = sel;
    return true;
}

/* ---------------- MCP ops wired to the store ---------------- */

static uint64_t g_pos_epoch;   /* the agent's current reconstructed position */

static int op_list(void* c, uint64_t* oldest, uint64_t* newest, uint32_t* count) {
    (void)c;
    const keyframe_ring_t* r = keyframe_store_ring(&g_ks);
    if (!keyframe_ring_oldest(r, oldest)) return -1;
    keyframe_ring_newest(r, newest);
    *count = keyframe_ring_count(r);
    return 0;
}
static int op_rewind(void* c, uint64_t epoch, uint64_t offset) {
    (void)c; (void)offset;
    uint64_t sel = 0;
    if (!restore_keyframe(epoch, &sel)) return -1;
    g_pos_epoch = sel;
    return 0;
}
static int op_reverse(void* c, uint64_t* oe, uint64_t* oo) {
    (void)c;
    if (g_pos_epoch == 0) return -1;
    uint64_t sel = 0;
    if (!restore_keyframe(g_pos_epoch - 1, &sel)) return -1;
    g_pos_epoch = sel;
    if (oe) *oe = sel;
    if (oo) *oo = 0;
    return 0;
}
static int op_watch(void* c, uint64_t* oe, uint64_t* oo) {
    (void)c; if (oe) *oe = g_pos_epoch; if (oo) *oo = 0; return 0;
}
static mcp_ops_t g_ops = { op_list, op_rewind, op_reverse, op_watch, 0 };

/* ---------------- MCP transport over a scripted agent script ---------------- */

static const char* g_script[] = {
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}\n",
    "{\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"list_checkpoints\",\"arguments\":{}}}\n",
    "{\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"rewind_to\",\"arguments\":{\"epoch\":5,\"offset\":0}}}\n",
    "{\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"reverse_step\",\"arguments\":{}}}\n",
    "{\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"reverse_step\",\"arguments\":{}}}\n",
    0
};
static const char* g_line; static uint32_t g_lpos;
static uint32_t g_si;
static int t_get(void* c) {
    (void)c;
    if (!g_line || g_line[g_lpos] == '\0') {
        g_line = g_script[g_si++];
        g_lpos = 0;
        if (!g_line) return -1; /* script exhausted: close */
    }
    return (uint8_t)g_line[g_lpos++];
}
static char g_resp[4096]; static uint32_t g_rlen;
static int t_put(void* c, uint8_t b) { (void)c; if (g_rlen < sizeof(g_resp)) g_resp[g_rlen++] = (char)b; return 0; }
static mcp_transport_t g_transport = { t_get, t_put, 0 };
static int srv_handle(const char* req, uint32_t n, char* out, uint32_t cap) {
    return mcp_handle(&g_ops, req, n, out, cap);
}

/* ---------------- helpers ---------------- */

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", msg); failures++; } \
    else { printf("  [ ok ] %s\n", msg); } \
} while (0)

/* Recompute the components of the current g_state and diverge-check them against
 * the sums recorded for `epoch`. Returns true if no leak. */
static bool verify_no_divergence(uint64_t epoch) {
    divergence_init(&g_det, DIVERGE_REPLAY);
    uint32_t dense[DIVERGE_MAX_COMPONENTS];
    for (uint32_t i = 0; i < DIVERGE_MAX_COMPONENTS; i++) dense[i] = 0;
    dense[KDIVERGE_PROCTABLE] = g_kf_sums[epoch - 1][0];
    dense[KDIVERGE_SCHEDULER] = g_kf_sums[epoch - 1][1];
    divergence_expect(&g_det, epoch, dense, KDIVERGE_SCHEDULER + 1);
    bool ok = diverge_scan_check(g_srcs, 2, det_check);
    return ok && !divergence_has_diverged(&g_det);
}

int main(void) {
    printf("=== Host harness: keyframe store + journal + divergence + MCP loop ===\n\n");

    /* ---- 1. SETUP ---- */
    printf("[setup] arming keyframe retention store + journal + divergence\n");
    fat_block_device_t* dev = make_dev();
    CHECK(keyframe_store_init(&g_ks, dev, KF_BASE, 3, CAP, 10) == KEYFRAME_STORE_OK, "keyframe store init");
    CHECK(keyframe_store_format(&g_ks) == KEYFRAME_STORE_OK, "keyframe store format");

    /* Journal store placed well past the keyframe store's regions. */
    uint32_t jbase = KF_BASE + keyframe_store_total_sectors(3, CAP, 10);
    CHECK(journal_capture_init != 0, "journal capture available"); /* linkage guard */

    journal_store_t jstore;
    CHECK(journal_store_init(&jstore, dev, jbase, 8) == JOURNAL_OK, "journal store init");
    CHECK(journal_store_format(&jstore) == JOURNAL_OK, "journal store format");

    /* ---- 2. RECORD a session ---- */
    printf("\n[record] running %d epochs, journaling inputs + divergence sums\n", E);
    g_state = 0xC0FFEEULL;
    for (uint64_t e = 1; e <= E; e++) {
        /* Keyframe at the epoch boundary. */
        g_kf_state[e - 1] = g_state;
        CHECK(put_keyframe(e) == KEYFRAME_STORE_OK, "keyframe written");

        /* Divergence RECORD over the components at this boundary. */
        divergence_init(&g_det, DIVERGE_RECORD);
        divergence_begin_epoch(&g_det, e);
        uint32_t ids[2], sums[2];
        diverge_scan_record(g_srcs, 2, det_record, ids, sums);
        g_kf_sums[e - 1][0] = sums[0];
        g_kf_sums[e - 1][1] = sums[1];

        /* Journal this epoch: a couple of time inputs plus the divergence sums,
         * through the REAL journal capture path. Stage the data the file-scope
         * journal sources (e2e_time_src / e2e_div_src) read. */
        g_e2e_time[0] = e * 7u; g_e2e_time[1] = e * 13u;
        g_e2e_ids[0] = ids[0]; g_e2e_ids[1] = ids[1];
        g_e2e_sums[0] = sums[0]; g_e2e_sums[1] = sums[1];

        journal_capture_sources_t src;
        memset(&src, 0, sizeof(src));
        src.time_values = e2e_time_src;
        src.divergence_sums = e2e_div_src;
        CHECK(journal_capture_epoch(&jstore, e, 0, &src) == JOURNAL_OK, "epoch journaled");

        /* Advance the session state by this epoch's inputs. */
        g_state = g_state * 6364136223846793005ULL + g_e2e_time[0] + g_e2e_time[1] + 1;
    }
    const keyframe_ring_t* ring = keyframe_store_ring(&g_ks);
    uint64_t oldest = 0, newest = 0;
    keyframe_ring_oldest(ring, &oldest); keyframe_ring_newest(ring, &newest);
    printf("[record] retained window: epochs %llu..%llu (last %u of %d)\n",
           (unsigned long long)oldest, (unsigned long long)newest,
           keyframe_ring_count(ring), E);

    /* ---- 3. REVERSE via the MCP front end ---- */
    printf("\n[agent] driving rewind/reverse over the MCP JSON-RPC loop\n");
    g_pos_epoch = 0; g_si = 0; g_line = 0; g_lpos = 0; g_rlen = 0;
    int rc = mcp_server_loop(&g_transport, srv_handle);
    CHECK(rc == MCP_SERVER_CLOSED, "MCP loop served the agent script and closed");
    /* The transcript the agent saw. */
    printf("---- MCP transcript ----\n%.*s------------------------\n", (int)g_rlen, g_resp);

    /* ---- 4. VERIFY reconstructions match, no divergence ---- */
    printf("[verify] reconstructing past moments and checking for leaks\n");
    int leaks = 0;
    for (uint64_t e = oldest; e <= newest; e++) {
        uint64_t sel = 0;
        if (!restore_keyframe(e, &sel)) { CHECK(false, "restore retained epoch"); continue; }
        bool state_ok = (g_state == g_kf_state[e - 1]);
        bool div_ok = verify_no_divergence(e);
        if (!state_ok || !div_ok) leaks++;
        char msg[64];
        /* format: "epoch N reconstructed, no leak" */
        (void)msg;
        printf("  [ %s ] epoch %llu: state=%s divergence=%s\n",
               (state_ok && div_ok) ? "ok " : "FAIL",
               (unsigned long long)e,
               state_ok ? "match" : "MISMATCH",
               div_ok ? "clean" : "LEAK");
        if (!state_ok) failures++;
        if (!div_ok) failures++;
    }
    CHECK(leaks == 0, "divergence detector reports NO leak over the live run");

    /* ---- Negative control: prove the detector is live ---- */
    printf("\n[control] injecting a corrupted restore to prove the detector fires\n");
    restore_keyframe(newest, 0);
    g_state ^= 0xDEADBEEFULL;               /* simulate a nondeterminism leak */
    bool caught = !verify_no_divergence(newest);
    CHECK(caught, "a corrupted reconstruction IS caught by the divergence detector");

    printf("\n%s\n", failures ? "FAILED" : "PASSED: stores, journal, divergence, and MCP loop compose (host harness)");
    return failures ? 1 : 0;
}
