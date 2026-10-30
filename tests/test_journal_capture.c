/* Host-side unit test for per-epoch journal capture (#194).
 *
 * Uses an in-memory mock block device (same convention as
 * test_checkpoint_journal.c) to verify that journal_capture_epoch:
 *   1. Writes scheduler points, time reads and entropy bytes in that order,
 *      each with the right event type / lclock / value.
 *   2. Reconstructs an entropy run that is not a multiple of 8 bytes exactly
 *      (via the per-event len field), across a full and a partial chunk.
 *   3. Skips a delta class whose source pointer is NULL.
 *   4. Round-trips crash-consistently: after capture the journal loads back
 *      with the committed epoch.
 *   5. Writes extended events (#241) verbatim, in order, after the entropy
 *      run and before the epoch length.
 *
 * Build: gcc -I../include -o test_journal_capture \
 *            test_journal_capture.c ../kernel/journal_capture.c \
 *            ../kernel/checkpoint_journal.c
 */

#include <stdint.h>
#include <stdbool.h>

/* Declare the libc bits we use directly, rather than including <string.h>/
 * <stdlib.h>/<stdio.h>. Those pull in <sys/types.h>, whose ssize_t typedef
 * conflicts with the one in IKOS's vfs.h (reached via fat.h). */
typedef __SIZE_TYPE__ size_t;
extern void* memcpy(void*, const void*, size_t);
extern void* memset(void*, int, size_t);
extern int   printf(const char*, ...);

#include "journal_capture.h"

/* ----- Mock block device backed by a flat buffer ----- */

#define MOCK_SECTORS 512
typedef struct {
    uint8_t data[MOCK_SECTORS * JOURNAL_SECTOR_SIZE];
} mock_dev_t;

static int mock_read(void* device, uint32_t sector, uint32_t count, void* buffer) {
    mock_dev_t* m = (mock_dev_t*)device;
    if ((uint64_t)sector + count > MOCK_SECTORS) return -1;
    memcpy(buffer, m->data + (size_t)sector * JOURNAL_SECTOR_SIZE,
           (size_t)count * JOURNAL_SECTOR_SIZE);
    return 0;
}
static int mock_write(void* device, uint32_t sector, uint32_t count, const void* buffer) {
    mock_dev_t* m = (mock_dev_t*)device;
    if ((uint64_t)sector + count > MOCK_SECTORS) return -1;
    memcpy(m->data + (size_t)sector * JOURNAL_SECTOR_SIZE, buffer,
           (size_t)count * JOURNAL_SECTOR_SIZE);
    return 0;
}

static mock_dev_t g_mock;
static fat_block_device_t g_bdev;

static fat_block_device_t* make_dev(void) {
    memset(&g_mock, 0, sizeof(g_mock));
    g_bdev.read_sectors = mock_read;
    g_bdev.write_sectors = mock_write;
    g_bdev.sector_size = JOURNAL_SECTOR_SIZE;
    g_bdev.total_sectors = MOCK_SECTORS;
    g_bdev.private_data = &g_mock;
    return &g_bdev;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

/* Store geometry: superblock + two slots, 4 sectors each (1 header + 3 event
 * sectors => 48 events per slot), enough to hold the fixtures below. */
#define BASE_SECTOR 10
#define SLOT_SECTORS 4

/* ----- Fake delta sources ----- */

static const uint64_t k_points[]  = { 5, 11, 23, 42 };
static const uint64_t k_times[]   = { 0x1111, 0x2222, 0x3333 };
/* 11 bytes: one full 8-byte chunk plus a 3-byte remainder. */
static const uint8_t  k_entropy[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03,
                                      0x04, 0xAA, 0xBB, 0xCC };

static uint32_t src_points(const uint64_t** out) {
    *out = k_points;
    return (uint32_t)(sizeof(k_points) / sizeof(k_points[0]));
}
static uint32_t src_times(const uint64_t** out) {
    *out = k_times;
    return (uint32_t)(sizeof(k_times) / sizeof(k_times[0]));
}
static uint32_t src_entropy(const uint8_t** out) {
    *out = k_entropy;
    return (uint32_t)sizeof(k_entropy);
}

/* Reassemble entropy bytes from an event (packed little-endian, len valid). */
static const journal_capture_event_t k_ext[] = {
    { JOURNAL_EV_EXTENDED_BASE + 0, 0, 15, 0xABCDEF0123456789ULL },
    { JOURNAL_EV_EXTENDED_BASE + 3, 5, 31, 0x0000000504030201ULL },
};
static uint32_t src_ext(const journal_capture_event_t** out) {
    *out = k_ext;
    return 2;
}
static uint64_t src_len(void) { return 77; }

static uint32_t unpack_entropy(const journal_event_t* ev, uint8_t* out) {
    for (uint32_t b = 0; b < ev->len; b++) {
        out[b] = (uint8_t)(ev->value >> (8u * b));
    }
    return ev->len;
}

int main(void) {
    printf("test_journal_capture\n");

    fat_block_device_t* dev = make_dev();
    journal_store_t store;
    CHECK(journal_store_init(&store, dev, BASE_SECTOR, SLOT_SECTORS) == JOURNAL_OK,
          "store init");
    CHECK(journal_store_format(&store) == JOURNAL_OK, "store format");

    /* --- 1/2/4: capture all three delta classes for epoch 7 --- */
    journal_capture_sources_t src = {
        .preempt_points = src_points,
        .time_values    = src_times,
        .entropy_bytes  = src_entropy,
    };
    CHECK(journal_capture_epoch(&store, 7, 100, &src) == JOURNAL_OK,
          "capture epoch 7");

    journal_reader_t rd;
    CHECK(journal_store_load(&store, &rd) == JOURNAL_OK, "load committed journal");
    CHECK(rd.epoch == 7, "loaded epoch is 7");

    /* Expected order: 4 sched points, then 3 time reads, then 2 entropy chunks
     * (8-byte full + 3-byte remainder) = 9 events. */
    CHECK(rd.event_count == 9, "event count is 9");

    journal_event_t ev;
    int idx = 0;
    uint8_t got_entropy[16];
    uint32_t got_entropy_len = 0;
    bool order_ok = true, sched_ok = true, timer_ok = true, entropy_type_ok = true;

    while (journal_reader_next(&rd, &ev) == JOURNAL_OK) {
        if (idx < 4) {
            if (ev.type != JOURNAL_EV_SCHED) sched_ok = false;
            if (ev.value != k_points[idx] || ev.lclock != k_points[idx])
                sched_ok = false;
        } else if (idx < 7) {
            if (ev.type != JOURNAL_EV_TIMER) timer_ok = false;
            if (ev.value != k_times[idx - 4] || ev.lclock != (uint64_t)(idx - 4))
                timer_ok = false;
        } else {
            if (ev.type != JOURNAL_EV_ENTROPY) entropy_type_ok = false;
            got_entropy_len += unpack_entropy(&ev, got_entropy + got_entropy_len);
        }
        idx++;
    }
    CHECK(idx == 9, "read back 9 events in order");
    CHECK(order_ok, "iteration completed");
    CHECK(sched_ok, "scheduler points: type/lclock/value correct");
    CHECK(timer_ok, "time reads: type/index/value correct");
    CHECK(entropy_type_ok, "entropy events tagged JOURNAL_EV_ENTROPY");

    /* Entropy stream reconstructed exactly, including the 3-byte remainder. */
    bool entropy_bytes_ok = (got_entropy_len == sizeof(k_entropy));
    for (uint32_t i = 0; i < sizeof(k_entropy) && entropy_bytes_ok; i++) {
        if (got_entropy[i] != k_entropy[i]) entropy_bytes_ok = false;
    }
    CHECK(entropy_bytes_ok, "entropy run reconstructed byte-for-byte (11 bytes)");

    /* --- 3: a NULL source class is skipped, others still recorded --- */
    make_dev();
    CHECK(journal_store_init(&store, dev, BASE_SECTOR, SLOT_SECTORS) == JOURNAL_OK,
          "re-init store");
    CHECK(journal_store_format(&store) == JOURNAL_OK, "re-format store");

    journal_capture_sources_t sched_only = {
        .preempt_points = src_points,
        .time_values    = NULL,
        .entropy_bytes  = NULL,
    };
    CHECK(journal_capture_epoch(&store, 8, 0, &sched_only) == JOURNAL_OK,
          "capture epoch 8 (sched only)");
    CHECK(journal_store_load(&store, &rd) == JOURNAL_OK, "load sched-only journal");
    CHECK(rd.event_count == 4, "sched-only journal has 4 events");
    bool all_sched = true;
    while (journal_reader_next(&rd, &ev) == JOURNAL_OK) {
        if (ev.type != JOURNAL_EV_SCHED) all_sched = false;
    }
    CHECK(all_sched, "sched-only journal holds only scheduler events");

    /* --- param guard --- */
    /* --- 5: extended events ride after entropy, before the epoch length --- */
    CHECK(journal_store_format(&store) == JOURNAL_OK, "re-format store (ext)");
    journal_capture_sources_t ext = {
        .entropy_bytes   = src_entropy,
        .epoch_length    = src_len,
        .extended_events = src_ext,
    };
    CHECK(journal_capture_epoch(&store, 9, 0, &ext) == JOURNAL_OK, "capture with extended events");
    CHECK(journal_store_load(&store, &rd) == JOURNAL_OK, "load ext journal");
    /* 2 entropy chunks, 2 extended events, 1 epoch length. */
    CHECK(rd.event_count == 5, "ext journal has 5 events");
    journal_event_t evs[5];
    for (int i = 0; i < 5; i++) journal_reader_next(&rd, &evs[i]);
    CHECK(evs[0].type == JOURNAL_EV_ENTROPY && evs[1].type == JOURNAL_EV_ENTROPY,
          "entropy first");
    CHECK(evs[2].type == k_ext[0].type && evs[2].lclock == 15 && evs[2].value == k_ext[0].value,
          "first extended event verbatim");
    CHECK(evs[3].type == k_ext[1].type && evs[3].len == 5 && evs[3].lclock == 31 &&
          evs[3].value == k_ext[1].value, "second extended event verbatim (len kept)");
    CHECK(evs[4].type == JOURNAL_EV_EPOCH_LEN && evs[4].value == 77, "epoch length last");

    CHECK(journal_capture_epoch(NULL, 1, 0, &src) == JOURNAL_ERR_PARAM,
          "NULL store rejected");
    CHECK(journal_capture_epoch(&store, 1, 0, NULL) == JOURNAL_ERR_PARAM,
          "NULL sources rejected");

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
