/* A two-process pipeline (#249): `producer | consumer`, set up the way a
 * shell does it (pipe, fork, dup2, execve), and checks of the pipe's edges.
 *
 *   musl_pipeline [bytes]        run the pipeline, then the edge checks
 *   musl_pipeline produce N      write N bytes of a known stream to stdout in
 *                                chunks of varying size, some larger than
 *                                PIPE_BUF (short writes are retried)
 *   musl_pipeline consume        read stdin to its end with read, poll, and
 *                                select; print the count and a checksum
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static unsigned char byte_at(unsigned long i) { return (unsigned char)((i * 2654435761u) >> 13); }

static int produce(unsigned long total) {
    static unsigned char buf[20000];
    unsigned long sent = 0;
    unsigned sizes[] = { 1, 100, 4096, 4097, 9000, 17, 20000, 512 };
    for (unsigned k = 0; sent < total; k++) {
        unsigned long n = sizes[k % 8];
        if (n > total - sent) n = total - sent;
        for (unsigned long i = 0; i < n; i++) buf[i] = byte_at(sent + i);
        unsigned long done = 0;
        while (done < n) {
            ssize_t w = write(1, buf + done, n - done);
            if (w < 0) {
                if (errno == EINTR) continue;
                return 2;
            }
            done += (unsigned long)w;
        }
        sent += n;
    }
    return 0;
}

static int consume(void) {
    static unsigned char buf[7000];
    unsigned long count = 0, bad = 0;
    unsigned long long h = 1469598103934665603ULL;
    unsigned polls = 0, selects = 0;
    for (unsigned k = 0;; k++) {
        if (k % 3 == 1) {
            struct pollfd p = { 0, POLLIN, 0 };
            if (poll(&p, 1, 5000) != 1 || !(p.revents & (POLLIN | POLLHUP))) return 3;
            polls++;
        } else if (k % 3 == 2) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(0, &r);
            struct timeval tv = { 5, 0 };
            if (select(1, &r, 0, 0, &tv) != 1 || !FD_ISSET(0, &r)) return 4;
            selects++;
        }
        ssize_t n = read(0, buf, 1 + (k * 977) % sizeof(buf));
        if (n < 0) return 5;
        if (n == 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != byte_at(count + (unsigned long)i)) bad++;
            h = (h ^ buf[i]) * 1099511628211ULL;
        }
        count += (unsigned long)n;
    }
    printf("consumer: %lu bytes, %lu wrong, hash %016llx, after %u polls and %u selects\n", count, bad, h,
           polls, selects);
    return bad ? 6 : 0;
}

/* The edges: EOF once the writer is gone, EPIPE once the reader is gone,
 * O_NONBLOCK, a poll that times out, and FIONREAD-free sizing by poll. */
static int edges(void) {
    int p[2];
    if (pipe(p) != 0) return 10;
    char c = 'x';
    if (write(p[1], &c, 1) != 1) return 11;
    close(p[1]);
    char got[4];
    if (read(p[0], got, 4) != 1 || read(p[0], got, 4) != 0) return 12;      /* then EOF */
    close(p[0]);

    if (pipe2(p, O_NONBLOCK) != 0) return 13;
    if (read(p[0], got, 4) != -1 || errno != EAGAIN) return 14;
    struct pollfd q = { p[0], POLLIN, 0 };
    if (poll(&q, 1, 20) != 0 || q.revents != 0) return 15;                     /* a timeout */
    signal(SIGPIPE, SIG_IGN);
    close(p[0]);
    if (write(p[1], &c, 1) != -1 || errno != EPIPE) return 16;
    close(p[1]);
    printf("pipeline: EOF, EAGAIN, a poll timeout, and EPIPE as expected\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "produce") == 0) return produce(strtoul(argv[2], 0, 10));
    if (argc > 1 && strcmp(argv[1], "consume") == 0) return consume();
    unsigned long total = argc > 1 ? strtoul(argv[1], 0, 10) : 300000;
    char amount[32];
    snprintf(amount, sizeof(amount), "%lu", total);
    int p[2];
    if (pipe(p) != 0) return 1;
    pid_t a = fork();
    if (a == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        char* args[] = { "musl_pipeline", "produce", amount, 0 };
        execve("musl_pipeline", args, environ);
        _exit(127);
    }
    pid_t b = fork();
    if (b == 0) {
        dup2(p[0], 0);
        close(p[0]);
        close(p[1]);
        char* args[] = { "musl_pipeline", "consume", 0 };
        execve("musl_pipeline", args, environ);
        _exit(127);
    }
    close(p[0]);
    close(p[1]);
    int sa, sb;
    waitpid(a, &sa, 0);
    waitpid(b, &sb, 0);
    printf("pipeline: producer exit %d, consumer exit %d\n", WEXITSTATUS(sa), WEXITSTATUS(sb));
    int e = edges();
    if (e) printf("pipeline: edge check %d failed\n", e);
    return WEXITSTATUS(sa) || WEXITSTATUS(sb) || e;
}
