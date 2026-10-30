/* Laplace core: processes that start processes (#248).
 *
 * fork, vfork, and clone without CLONE_VM copy the address space lazily:
 * both spaces map every frame read-only with PAGE_FORK_COW, the frame's
 * reference count (frame_share) says how many map it, and the first write
 * in either copies it. Snapshot COW (keyframes) and fork COW meet in one
 * write-fault path, vmm_user_write_fault: the keyframe image is preserved
 * first, then the frame is unshared. A keyframe stores a shared frame once.
 *
 * execve replaces the program through the loader (#240); close-on-exec
 * descriptors close. A process that exits while its parent lives becomes a
 * zombie until wait4 reaps it; a parent waiting for it is woken at once.
 * Orphans go to pid 1. kill sends a signal (include/core/ksig.h). pids stay
 * deterministic: the counter is kernel state.
 *
 * Threads (#251): clone with CLONE_VM|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD
 * (what musl's pthread_create passes) adds a scheduling unit to the
 * caller's thread group: its own registers, extended state, FS base, signal
 * mask, and pending signals; the group's address space, descriptor table,
 * and handlers. getpid is the group's id (the leader's pid), gettid the
 * thread's. exit ends one thread (clearing and waking its
 * CLONE_CHILD_CLEARTID word, for pthread_join); exit_group, a fatal
 * signal, or the last thread's exit ends the process. A leader whose own
 * thread exits stays in the table, parked, holding the group's pid and
 * address space. futex WAIT/WAKE (and the private, bitset, and requeue
 * variants) block in WAIT_FUTEX on (address space, address); waiters wake
 * in table order, so wakeups replay.
 */

#ifndef CORE_PCTL_H
#define CORE_PCTL_H

#include <stdbool.h>
#include <stdint.h>
#include "process.h"
#include "core/linux.h"

/* Handle a process-control system call. False if `nr` is not one. */
bool pctl_syscall(process_t* p, uint64_t nr, lx_after_t* after);

/* A child became a zombie: reap it into a parent waiting for it. */
void pctl_child_exited(process_t* c);

#endif /* CORE_PCTL_H */
