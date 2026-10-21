/* Processes starting processes (#248).
 *
 *   forkexec parent N [pause]   fork N children; each changes a shared
 *                               array (copy-on-write: the parent must not see
 *                               it), then execs `forkexec child I`, which
 *                               exits with I. The parent waits for all and
 *                               checks every status. With `pause`, each child
 *                               sleeps 300 ms between fork and exec, and
 *                               execs musl_hello instead.
 *   forkexec child I P          check that the parent is P, exit with I
 *   forkexec kill               fork a sleeper, SIGTERM it, reap it
 */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint32_t g_shared[4096];   /* 16 KiB the children write after fork */
extern char** environ;

int main(int argc, char** argv) {
    if (argc >= 4 && strcmp(argv[1], "child") == 0) {
        if (getppid() != atoi(argv[3])) return 250;   /* the parent, across execve */
        return atoi(argv[2]) & 0xFF;
    }
    if (argc >= 2 && strcmp(argv[1], "kill") == 0) {
        pid_t c = fork();
        if (c == 0) {
            struct timespec d = { 10, 0 };
            nanosleep(&d, 0);
            return 0;
        }
        struct timespec d = { 0, 20000000 };
        nanosleep(&d, 0);
        if (kill(c, SIGTERM) != 0) { printf("forkexec: kill failed\n"); return 1; }
        int st = 0;
        pid_t w = waitpid(c, &st, 0);
        printf("forkexec: killed pid %d: %s %d\n", (int)w,
               WIFSIGNALED(st) ? "signal" : "status", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "parent") == 0) {
        int n = atoi(argv[2]);
        int pause = argc >= 4 && strcmp(argv[3], "pause") == 0;
        for (int i = 0; i < 4096; i++) g_shared[i] = (uint32_t)i;
        for (int i = 0; i < n; i++) {
            pid_t c = fork();
            if (c < 0) { printf("forkexec: fork %d failed\n", i); return 1; }
            if (c == 0) {
                for (int k = 0; k < 4096; k++) g_shared[k] = 0xDEAD0000u + (uint32_t)i;
                if (pause) {
                    struct timespec d = { 0, 300000000 };
                    nanosleep(&d, 0);
                }
                char num[16], parent[16];
                snprintf(num, sizeof(num), "%d", i);
                snprintf(parent, sizeof(parent), "%d", (int)getppid());
                char* args[] = { "musl_forkexec", "child", num, parent, 0 };
                char* hello[] = { "musl_hello", 0 };
                /* With pause, run another program: between fork and exec the
                 * child is still musl_forkexec, after it musl_hello. */
                if (pause) execve("musl_hello", hello, environ);
                else execve("musl_forkexec", args, environ);
                _exit(254);   /* exec failed */
            }
        }
        int seen = 0, bad = 0;
        long sum = 0;
        for (int i = 0; i < n; i++) {
            int st = 0;
            pid_t w = wait(&st);
            if (w <= 0) { printf("forkexec: wait failed after %d\n", seen); return 1; }
            if (!WIFEXITED(st) || WEXITSTATUS(st) >= 250) bad++;
            sum += WEXITSTATUS(st);
            seen++;
        }
        int intact = 1;
        for (int k = 0; k < 4096; k++) intact &= g_shared[k] == (uint32_t)k;
        int st;
        int extra = wait(&st);
        printf("forkexec: %d children forked, exec'd, and reaped; exit codes sum %ld; %d bad; "
               "parent memory %s; then wait says %s\n", seen, sum, bad, intact ? "intact" : "CHANGED",
               extra < 0 ? "no children" : "more");
        return 0;
    }
    printf("usage: forkexec parent N [pause] | child I | kill\n");
    return 2;
}
