/* Laplace system call ABI (#220), shared by the kernel and user programs.
 *
 * int 0x80; number in rax, arguments in rdi, rsi, rdx; result in rax.
 *
 * Every system call is one step of the recorded machine: the logical clock
 * advances once per kernel entry (#222). The only nondeterministic inputs a
 * program can observe are SYS_TIME and SYS_RANDOM, both journaled, plus where
 * the scheduler preempts it, which is journaled too.
 */

#ifndef LAPLACE_SYSCALLS_H
#define LAPLACE_SYSCALLS_H

#define SYS_EXIT    0   /* exit(code): never returns */
#define SYS_WRITE   1   /* write(buf, len) -> bytes written to the console */
#define SYS_YIELD   2   /* yield(): give the CPU to the next process */
#define SYS_GETPID  3   /* getpid() -> pid */
#define SYS_TIME    4   /* time() -> cycle counter (recorded / replayed) */
#define SYS_RANDOM  5   /* random(buf, len) -> 0; fills buf (recorded / replayed) */
#define SYS_BREAK   6   /* break(code): stop the machine for the debugger */

#endif /* LAPLACE_SYSCALLS_H */
