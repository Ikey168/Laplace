/* Unit tests for the journal ring (#223): one journal per retained keyframe,
 * reused with its keyframe slot, never served stale, and reloadable after a
 * reboot.
 *
 * Build: gcc -Iinclude -o t tests/test_journal_ring.c kernel/journal_ring.c \
 *          kernel/checkpoint_journal.c kernel/journal_capture.c kernel/keyframe_ring.c
 */

#include <stdint.h>
#include <stdbool.h>

typedef __SIZE_TYPE__ size_t;
extern int   printf(const char*, ...);
extern void* memcpy(void*, const void*, size_t);

#include "journal_ring.h"
#include "journal_capture.h"

static int failures = 0;
#define CHECK(c, msg) do { if (c) printf("  ok:   %s\n", msg); \
    else { printf("  FAIL: %s\n", msg); failures++; } } while (0)

#define SECTORS 8192
static uint8_t g_disk[SECTORS * 512];
static int dev_read(void* d, uint32_t s, uint32_t n, void* b) {
    (void)d; if (s + n > SECTORS) return -1;
    memcpy(b, g_disk + (size_t)s * 512, (size_t)n * 512); return 0;
}
static int dev_write(void* d, uint32_t s, uint32_t n, const void* b) {
    (void)d; if (s + n > SECTORS) return -1;
    memcpy(g_disk + (size_t)s * 512, b, (size_t)n * 512); return 0;
}
static fat_block_device_t g_dev = { dev_read, dev_write, 512, SECTORS, 0 };

/* A fake epoch's deltas: the time value and length encode the epoch. */
static uint64_t g_times[3];
static uint64_t g_len;
static uint32_t src_times(const uint64_t** out) { *out = g_times; return 3; }
static uint64_t src_len(void) { return g_len; }

static int write_journal(journal_ring_t* jr, keyframe_ring_t* kr, uint64_t epoch) {
    journal_store_t* js = journal_ring_store_for(jr, kr, epoch);
    if (!js) return -1;
    for (int i = 0; i < 3; i++) g_times[i] = epoch * 100 + (uint64_t)i;
    g_len = epoch * 10;
    journal_capture_sources_t src = { 0 };
    src.time_values = src_times;
    src.epoch_length = src_len;
    return journal_capture_epoch(js, epoch, 0, &src);
}

/* Load epoch's journal and check it carries that epoch's data. */
static int check_journal(journal_ring_t* jr, keyframe_ring_t* kr, uint64_t epoch) {
    journal_reader_t rd;
    if (journal_ring_load(jr, kr, epoch, &rd) != JOURNAL_RING_OK) return -1;
    journal_event_t ev;
    int times = 0;
    uint64_t len = 0;
    while (journal_reader_next(&rd, &ev) == JOURNAL_OK) {
        if (ev.type == JOURNAL_EV_TIMER && ev.value == epoch * 100 + (uint64_t)times) times++;
        if (ev.type == JOURNAL_EV_EPOCH_LEN) len = ev.value;
    }
    return (times == 3 && len == epoch * 10) ? 0 : -2;
}

int main(void) {
    keyframe_ring_t kr;
    journal_ring_t jr;

    printf("Test 1: geometry and format\n");
    CHECK(journal_ring_total_sectors(3, 16) == 3 * 33, "total sectors = capacity * (1 + 2 * slot)");
    CHECK(journal_ring_init(&jr, &g_dev, 100, 3, 16) == JOURNAL_RING_OK, "init");
    CHECK(journal_ring_init(&jr, &g_dev, 100, 0, 16) == JOURNAL_RING_ERR_PARAM, "zero capacity rejected");
    CHECK(journal_ring_init(&jr, &g_dev, 100, 3, 16) == JOURNAL_RING_OK, "re-init");
    CHECK(journal_ring_format(&jr) == JOURNAL_RING_OK, "format all regions");
    keyframe_ring_init(&kr, 3);

    printf("Test 2: one journal per retained keyframe\n");
    for (uint64_t e = 1; e <= 3; e++) keyframe_ring_advance(&kr, e);
    CHECK(write_journal(&jr, &kr, 1) == JOURNAL_OK, "journal 1 written");
    CHECK(write_journal(&jr, &kr, 2) == JOURNAL_OK, "journal 2 written");
    CHECK(check_journal(&jr, &kr, 1) == 0, "journal 1 reads back epoch 1's inputs");
    CHECK(check_journal(&jr, &kr, 2) == 0, "journal 2 reads back epoch 2's inputs");
    CHECK(check_journal(&jr, &kr, 3) != 0, "journal 3 not written yet: not served");
    CHECK(write_journal(&jr, &kr, 3) == JOURNAL_OK, "journal 3 written");
    CHECK(check_journal(&jr, &kr, 3) == 0, "journal 3 reads back");
    CHECK(journal_ring_store_for(&jr, &kr, 7) == 0, "unretained epoch has no region");

    printf("Test 3: eviction reuses the region, never serves a stale journal\n");
    keyframe_ring_advance(&kr, 4);  /* evicts epoch 1, reuses its slot */
    CHECK(check_journal(&jr, &kr, 1) != 0, "evicted epoch 1: journal unavailable");
    CHECK(check_journal(&jr, &kr, 4) != 0, "epoch 4's region still holds epoch 1: not served as 4");
    CHECK(write_journal(&jr, &kr, 4) == JOURNAL_OK, "journal 4 written into the reused region");
    CHECK(check_journal(&jr, &kr, 4) == 0, "journal 4 reads back");
    CHECK(check_journal(&jr, &kr, 2) == 0, "journal 2 untouched by the reuse");

    printf("Test 4: an open epoch's journal can be rewritten (sealed, then continued)\n");
    g_len = 0;
    for (int round = 0; round < 2; round++) {
        write_journal(&jr, &kr, 4);
    }
    CHECK(check_journal(&jr, &kr, 4) == 0, "rewritten journal 4 still reads back");

    printf("Test 5: reload after a reboot\n");
    static uint8_t packed[4096];
    int plen = keyframe_ring_pack(&kr, packed, sizeof(packed));
    CHECK(plen > 0 && (uint32_t)plen == keyframe_ring_packed_size(), "ring index packed");
    keyframe_ring_t kr2;
    journal_ring_t jr2;
    CHECK(keyframe_ring_unpack(&kr2, packed, (uint32_t)plen) == KEYFRAME_RING_OK, "ring index unpacked");
    journal_ring_init(&jr2, &g_dev, 100, 3, 16);
    CHECK(check_journal(&jr2, &kr2, 2) == 0, "journal 2 found after reboot");
    CHECK(check_journal(&jr2, &kr2, 3) == 0, "journal 3 found after reboot");
    CHECK(check_journal(&jr2, &kr2, 4) == 0, "journal 4 found after reboot");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
