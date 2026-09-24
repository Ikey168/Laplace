/* Laplace core: user processes and the round-robin run order (#220).
 *
 * Processes are ring-3 programs loaded from ELF images embedded in the kernel.
 * The process table's order is the scheduler's round-robin order; it is part of
 * every keyframe, so replay walks the processes in the recorded order.
 *
 * The pm_* / process_* functions the checkpoint engine and the divergence
 * sources call (include/process.h, include/process_manager.h) are implemented
 * in kernel/core/proc.c over this table.
 */

#ifndef CORE_PROC_H
#define CORE_PROC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "process.h"
#include "core/cpu.h"

#define LP_MAX_PROCS     16
#define LP_STACK_TOP     0x80000000ULL
#define LP_STACK_PAGES   4

/* An embedded user program (user_images.asm). */
typedef struct {
    const char*    name;
    const uint8_t* start;
    const uint8_t* end;
} lp_image_t;

const lp_image_t* proc_find_image(const char* name);
const lp_image_t* proc_images(uint32_t* count);

/* Create a process from an embedded image, append it to the table (READY).
 * Returns NULL on a bad image or out of memory. */
process_t* proc_spawn(const lp_image_t* image);

/* Remove a process from the table and free it (address space included). */
void proc_destroy(process_t* p);
void proc_destroy_all(void);

uint32_t   proc_count(void);
process_t* proc_at(uint32_t index);        /* table order */
process_t* proc_by_pid(uint32_t pid);
int        proc_index(const process_t* p); /* -1 if not in the table */

/* Next READY process after `p` in table order (wrapping, `p` itself last);
 * NULL if none is READY. p may be NULL (start from the head). */
process_t* proc_next_ready(const process_t* p);

/* pid counter, captured in keyframes so a restored machine keeps numbering. */
uint32_t proc_next_pid(void);
void     proc_set_next_pid(uint32_t pid);

/* Build an empty process with a given pid (restore path). */
process_t* proc_alloc(uint32_t pid, const char* name);
void       proc_append(process_t* p);

/* Frame <-> saved context. */
void ctx_from_frame(process_context_t* c, const trap_frame_t* f);
void ctx_to_frame(const process_context_t* c, trap_frame_t* f);

#endif /* CORE_PROC_H */
