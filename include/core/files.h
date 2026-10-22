/* Laplace core: files and file descriptors for Linux programs (#247).
 *
 * The machine's filesystem is a memfs (include/memfs.h) seeded from a cpio
 * Multiboot module: the one named by `root=` on the kernel command line, else
 * the first module that is a cpio archive. Without one it is an empty
 * root with /tmp.
 *
 * Open file descriptions (offset, status flags, what they refer to) are
 * shared by dup'd descriptors; each process has a descriptor table, a working
 * directory, and a umask. Besides memfs files and directories, a description
 * can be the console (fds 0-2 of every process), /dev/null, /dev/zero, or
 * /dev/urandom (journaled entropy).
 *
 * All of it is kernel state (#243): sections "fs" (the tree; file pages go in
 * the keyframe page pool, #244) and "files" (descriptions, tables, cwds),
 * each its own divergence component. File times come from the last
 * wall-clock time the machine read through a journaled call, so they replay.
 */

#ifndef CORE_FILES_H
#define CORE_FILES_H

#include <stdint.h>
#include <stdbool.h>
#include "process.h"
#include "core/proc.h"

#define KS_ID_FS     6
#define KS_ID_FILES  7

/* Build the filesystem from the seed (may be NULL) and register its keyframe
 * sections. Once, at boot, after kstate_init. */
void files_init(const uint8_t* seed, uint64_t len, const char* seed_name);

/* Process lifecycle: a new process gets the console on fds 0-2 and / as its
 * cwd; a destroyed one closes everything. */
void files_proc_new(process_t* p);
void files_proc_gone(process_t* p);
/* fork: the child shares the parent's open descriptions (#248). */
void files_fork(process_t* parent, process_t* child);
/* A thread (#251): the child uses the parent's descriptor table. */
void files_share(process_t* parent, process_t* child);
/* execve: close the close-on-exec descriptors. */
void files_exec(process_t* p);

/* Handle a Linux file system call. False if `nr` is not one. */
bool files_syscall(process_t* p, uint64_t nr, int64_t* ret);

/* The machine read the wall clock (journaled): file times follow it. */
void files_note_time(uint64_t realtime_ns);

/* A program image stored in the filesystem (an untouched seed file), for
 * run= and execve. */
const lp_image_t* files_find_image(const char* path);

#endif /* CORE_FILES_H */
