/* Laplace user runtime: system call wrappers and tiny print helpers (#220).
 * Header-only; every program links as one translation unit plus this header. */

#ifndef LAPLACE_ULIB_H
#define LAPLACE_ULIB_H

#include <stdint.h>
#include "laplace/syscalls.h"

static inline long lp_syscall3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "memory");
    return ret;
}

static inline __attribute__((noreturn)) void sys_exit(int code) {
    lp_syscall3(SYS_EXIT, code, 0, 0);
    for (;;) { }
}
static inline long sys_write(const void* buf, unsigned long len) {
    return lp_syscall3(SYS_WRITE, (long)buf, (long)len, 0);
}
static inline void sys_yield(void) { lp_syscall3(SYS_YIELD, 0, 0, 0); }
static inline long sys_getpid(void) { return lp_syscall3(SYS_GETPID, 0, 0, 0); }
static inline uint64_t sys_time(void) { return (uint64_t)lp_syscall3(SYS_TIME, 0, 0, 0); }
static inline void sys_random(void* buf, unsigned long len) {
    lp_syscall3(SYS_RANDOM, (long)buf, (long)len, 0);
}
static inline void sys_break(long code) { lp_syscall3(SYS_BREAK, code, 0, 0); }

static inline unsigned long u_strlen(const char* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static inline void u_puts(const char* s) { sys_write(s, u_strlen(s)); }

static inline void u_put_u64(uint64_t v) {
    char buf[24];
    int n = 0;
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char out[24];
    for (int i = 0; i < n; i++) out[i] = buf[n - 1 - i];
    sys_write(out, (unsigned long)n);
}

static inline void u_put_hex(uint64_t v) {
    char out[18];
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int d = (int)((v >> (60 - 4 * i)) & 0xF);
        out[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    sys_write(out, 18);
}

/* Stop the machine for the debugger if `cond` is false. */
#define u_assert(cond, code) do { if (!(cond)) sys_break(code); } while (0)

int main(void);

/* Entry point: the loader starts every program here. */
__attribute__((section(".text.start"), used, noreturn))
void _start(void) {
    sys_exit(main());
}

#endif /* LAPLACE_ULIB_H */
