/* Host-side unit test for the generic input journal (#242).
 *
 * Verifies:
 *   1. The record format: records with and without payloads become journal
 *      events, and a fresh log loaded from those events (in any order)
 *      returns the same records and the same payload bytes.
 *   2. Payload splitting: payloads cross page boundaries, a 10 MiB payload
 *      fills 2560 pages, and its pages cost a handful of events (runs).
 *   3. The mismatch check: a replay whose next call has another step, kind,
 *      call number, or argument digest is told so, and nothing is consumed;
 *      running past the last record says so.
 *   4. Continuing a replayed epoch as a recording appends after it.
 *
 * Build: gcc -I../include -o test_iorec test_iorec.c ../kernel/iorec.c
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
extern int printf(const char*, ...);

#include "iorec.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

/* A page store in memory. */
#define MAXP 4096
static uint8_t* g_store[MAXP];
static uint32_t g_next;
static int g_reads;
static uint32_t st_alloc(void* c) { (void)c; return g_next < MAXP ? g_next++ : IOREC_NONE; }
static int st_write(void* c, uint32_t id, const uint8_t* p) {
    (void)c;
    g_store[id] = malloc(IOREC_PAGE);
    memcpy(g_store[id], p, IOREC_PAGE);
    return 0;
}
static int st_read(void* c, uint32_t id, uint8_t* p) {
    (void)c;
    g_reads++;
    if (id >= MAXP || !g_store[id]) return -1;
    memcpy(p, g_store[id], IOREC_PAGE);
    return 0;
}
static void* m_alloc(uint64_t n) { return malloc(n); }
static void m_free(void* p) { free(p); }
static const iorec_env_t ENV = { st_alloc, st_write, st_read, m_alloc, m_free, 0 };

static iorec_event_t g_ev[100000];
static uint32_t g_nev;
static void drain(iorec_log_t* l) { g_nev += iorec_take_events(l, g_ev + g_nev, 100000 - g_nev); }

static iorec_log_t rec, rep;

int main(void) {
    printf("=== Input journal (#242) unit test ===\n");
    iorec_init(&rec, &ENV);

    /* --- 1/2. record --- */
    uint8_t small[13], mid[5000], *big = malloc(10u << 20);
    for (int i = 0; i < 13; i++) small[i] = (uint8_t)(i * 7);
    for (int i = 0; i < 5000; i++) mid[i] = (uint8_t)(i ^ 0x5A);
    for (uint32_t i = 0; i < (10u << 20); i++) big[i] = (uint8_t)(i * 2654435761u >> 24);
    CHECK(iorec_append(&rec, 3, IOREC_TIME, 0, 0, 123456789, 0, 0) == IOREC_OK, "a clock read");
    CHECK(iorec_append(&rec, 3, IOREC_ENTROPY, 0, 13, 0, small, 13) == IOREC_OK, "13 random bytes");
    CHECK(iorec_append(&rec, 7, IOREC_SYSCALL, 0, 0xABCD, 5000, mid, 5000) == IOREC_OK,
          "a 5000-byte read: crosses a page boundary");
    drain(&rec);
    CHECK(iorec_append(&rec, 9, IOREC_SYSCALL, 0, 0x1234, 10 << 20, big, 10u << 20) == IOREC_OK,
          "a 10 MiB read in one record");
    CHECK(iorec_append(&rec, 9, IOREC_SYSCALL, 1, 7, -11, 0, 0) == IOREC_OK, "an error result");
    CHECK(iorec_flush(&rec) == IOREC_OK, "the tail page is written at the epoch's end");
    drain(&rec);
    uint32_t page_events = 0;
    for (uint32_t i = 0; i < g_nev; i++) page_events += g_ev[i].type == IOREC_EV_PAGE;
    const uint32_t* ids;
    uint32_t npages = iorec_pages(&rec, &ids);
    CHECK(npages == (13 + 5000 + (10u << 20) + 4095) / 4096, "the stream fills whole pages");
    CHECK(page_events <= 3, "consecutive pages are one event per run");
    printf("  (%u records, %u pages, %u events)\n", iorec_count(&rec), npages, g_nev);

    /* Load in reverse order of types: pages first. */
    iorec_init(&rep, &ENV);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < g_nev; i++)
            if ((g_ev[i].type == IOREC_EV_PAGE) == (pass == 0))
                iorec_load_event(&rep, g_ev[i].type, g_ev[i].len, g_ev[i].lclock, g_ev[i].value);
    iorec_t r;
    uint8_t buf[5000];
    CHECK(iorec_next(&rep, 3, IOREC_TIME, 0, 0, &r) == IOREC_OK && r.ret == 123456789, "the clock read replays");
    CHECK(iorec_next(&rep, 3, IOREC_ENTROPY, 0, 13, &r) == IOREC_OK && r.len == 13 &&
          iorec_read(&rep, r.off, buf, r.len) == IOREC_OK && memcmp(buf, small, 13) == 0,
          "the random bytes replay");
    /* --- 3. mismatch --- */
    CHECK(iorec_next(&rep, 8, IOREC_SYSCALL, 0, 0xABCD, &r) == IOREC_MISMATCH && r.step == 7,
          "another step is a mismatch, reported with the recorded record");
    CHECK(iorec_next(&rep, 7, IOREC_SYSCALL, 1, 0xABCD, &r) == IOREC_MISMATCH, "another call number too");
    CHECK(iorec_next(&rep, 7, IOREC_SYSCALL, 0, 0xABCE, &r) == IOREC_MISMATCH, "other arguments too");
    CHECK(iorec_next(&rep, 7, IOREC_TIME, 0, 0xABCD, &r) == IOREC_MISMATCH, "another kind too");
    CHECK(iorec_next(&rep, 7, IOREC_SYSCALL, 0, 0xABCD, &r) == IOREC_OK && r.ret == 5000 &&
          iorec_read(&rep, r.off, buf, 5000) == IOREC_OK && memcmp(buf, mid, 5000) == 0,
          "a mismatch consumes nothing; the right call gets its payload");
    CHECK(iorec_next(&rep, 9, IOREC_SYSCALL, 0, 0x1234, &r) == IOREC_OK && r.len == (10u << 20),
          "the 10 MiB record");
    uint8_t* back = malloc(10u << 20);
    g_reads = 0;
    CHECK(iorec_read(&rep, r.off, back, r.len) == IOREC_OK && memcmp(back, big, 10u << 20) == 0,
          "its 10 MiB come back byte for byte");
    CHECK(g_reads <= 2562, "each page is read once");
    CHECK(iorec_next(&rep, 9, IOREC_SYSCALL, 1, 7, &r) == IOREC_OK && r.ret == -11, "an error result replays");
    CHECK(iorec_next(&rep, 10, IOREC_TIME, 0, 0, &r) == IOREC_EXHAUSTED, "past the end says so");

    /* --- 4. continue --- */
    iorec_continue(&rep);
    uint64_t before = iorec_bytes(&rep);
    CHECK(before % IOREC_PAGE == 0, "the continuation starts at a page boundary");
    g_nev = 0;
    CHECK(iorec_append(&rep, 11, IOREC_ENTROPY, 0, 4, 0, "abcd", 4) == IOREC_OK && iorec_flush(&rep) == IOREC_OK,
          "recording continues");
    drain(&rep);
    CHECK(g_nev == 3, "only the new record and page become events");
    iorec_log_t again;
    iorec_init(&again, &ENV);
    for (uint32_t i = 0; i < g_nev; i++) iorec_load_event(&again, g_ev[i].type, g_ev[i].len, g_ev[i].lclock, g_ev[i].value);
    char four[4];
    CHECK(iorec_next(&again, 11, IOREC_ENTROPY, 0, 4, &r) == IOREC_OK && r.off == before &&
          iorec_read(&again, r.off, four, 4) == IOREC_OK && memcmp(four, "abcd", 4) == 0,
          "and its payload sits after the old stream");

    free(big);
    free(back);
    if (failures == 0) {
        printf("PASSED: inputs record with payloads of any size, and a replay that strays is caught\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
