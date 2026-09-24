/* IKOS Orthogonal Persistence - Divergence Detector, kernel adapter (#166)
 *
 * A global divergence detector and the component ids the replay driver
 * checksums at each epoch boundary. During a record run the driver checksums
 * each component and calls kdiverge_record(); during replay it recomputes and
 * calls kdiverge_check(). In debug builds the driver asserts on kdiverge_ok()
 * so a nondeterminism leak halts replay at the exact epoch and component.
 *
 * The component checksums themselves are computed by the driver over the
 * restored subsystems (process table, scheduler, user pages, ...); this adapter
 * provides only the ids and the record/compare plumbing, so it stays
 * dependency-free.
 */

#include "divergence.h"

static divergence_t g_kdiverge;

void kdiverge_init(void) {
    divergence_init(&g_kdiverge, DIVERGE_OFF);
}

void kdiverge_set_mode(diverge_mode_t mode) {
    divergence_set_mode(&g_kdiverge, mode);
}

void kdiverge_begin_epoch(uint64_t epoch) {
    divergence_begin_epoch(&g_kdiverge, epoch);
}

int kdiverge_record(uint32_t component, uint32_t checksum) {
    return divergence_record(&g_kdiverge, component, checksum);
}

int kdiverge_expect(uint64_t epoch, const uint32_t* sums, uint32_t n) {
    return divergence_expect(&g_kdiverge, epoch, sums, n);
}

bool kdiverge_check(uint32_t component, uint32_t checksum) {
    return divergence_check(&g_kdiverge, component, checksum);
}

bool kdiverge_ok(void) {
    return !divergence_has_diverged(&g_kdiverge);
}

void kdiverge_reset(void) {
    diverge_mode_t mode = g_kdiverge.mode;
    divergence_init(&g_kdiverge, mode);
}

const divergence_t* kdiverge_detector(void) {
    return &g_kdiverge;
}
