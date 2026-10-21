/* Laplace core: the Linux x86-64 system call interface (#238).
 *
 * Programs built for Linux enter the kernel with the SYSCALL instruction;
 * the entry stub (isr.asm) builds the same trap frame `int 0x80` does, with
 * vector VEC_LINUX_SYSCALL, so a Linux system call is a step like any other
 * kernel entry. The number is in rax, arguments in rdi, rsi, rdx, r10, r8,
 * r9; the result (or -errno) goes back in rax; rcx and r11 hold the return
 * address and flags, as the CPU left them.
 *
 * Everything a call returns is a function of the machine's state, or comes
 * from a journaled source (ktime_read, kentropy_fill), so Linux programs
 * replay like the embedded ones. An unimplemented call returns -ENOSYS and is
 * reported once, with the caller's address.
 */

#ifndef CORE_LINUX_H
#define CORE_LINUX_H

#include <stdint.h>
#include <stdbool.h>
#include "process.h"

/* System call numbers (x86-64). */
#define LX_read             0
#define LX_write            1
#define LX_open             2
#define LX_close            3
#define LX_stat             4
#define LX_fstat            5
#define LX_lstat            6
#define LX_poll             7
#define LX_lseek            8
#define LX_mmap             9
#define LX_mprotect         10
#define LX_munmap           11
#define LX_brk              12
#define LX_rt_sigaction     13
#define LX_rt_sigprocmask   14
#define LX_rt_sigreturn     15
#define LX_ioctl            16
#define LX_pread64          17
#define LX_pwrite64         18
#define LX_readv            19
#define LX_writev           20
#define LX_access           21
#define LX_pipe             22
#define LX_select           23
#define LX_sched_yield      24
#define LX_mremap           25
#define LX_madvise          28
#define LX_dup              32
#define LX_dup2             33
#define LX_nanosleep        35
#define LX_getpid           39
#define LX_clone            56
#define LX_fork             57
#define LX_vfork            58
#define LX_execve           59
#define LX_exit             60
#define LX_wait4            61
#define LX_kill             62
#define LX_uname            63
#define LX_fcntl            72
#define LX_getcwd           79
#define LX_chdir            80
#define LX_gettimeofday     96
#define LX_getrlimit        97
#define LX_getuid           102
#define LX_getgid           104
#define LX_geteuid          107
#define LX_getegid          108
#define LX_setpgid          109
#define LX_getppid          110
#define LX_getpgrp          111
#define LX_setsid           112
#define LX_sigaltstack      131
#define LX_arch_prctl       158
#define LX_gettid           186
#define LX_time             201
#define LX_futex            202
#define LX_sched_getaffinity 204
#define LX_set_tid_address  218
#define LX_clock_gettime    228
#define LX_clock_getres     229
#define LX_clock_nanosleep  230
#define LX_exit_group       231
#define LX_tgkill           234
#define LX_openat           257
#define LX_newfstatat       262
#define LX_set_robust_list  273
#define LX_pipe2            293
#define LX_prlimit64        302
#define LX_getrandom        318
#define LX_statx            332
#define LX_rseq             334
#define LX_clone3           435

/* errno values. */
#define LX_EPERM   1
#define LX_ENOENT  2
#define LX_ESRCH   3
#define LX_EINTR   4
#define LX_EIO     5
#define LX_EBADF   9
#define LX_ECHILD  10
#define LX_EAGAIN  11
#define LX_ENOMEM  12
#define LX_EACCES  13
#define LX_EFAULT  14
#define LX_EEXIST  17
#define LX_ENOTDIR 20
#define LX_EISDIR  21
#define LX_EINVAL  22
#define LX_EMFILE  24
#define LX_ENOTTY  25
#define LX_ESPIPE  29
#define LX_EPIPE   32
#define LX_ERANGE  34
#define LX_ENOSYS  38
#define LX_ENOTEMPTY 39

/* What the machine does after a call: keep running the caller, or the call
 * itself changed the running process (exit, yield). */
typedef enum { LX_CONTINUE = 0, LX_SWITCHED } lx_after_t;

lx_after_t linux_syscall(process_t* p);

/* The pending Linux call ends the calling process (exit, exit_group). */
bool linux_is_exit(uint64_t nr);

#endif /* CORE_LINUX_H */
