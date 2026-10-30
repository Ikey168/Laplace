/* A loopback client and server inside the machine (#253): TCP over
 * 127.0.0.1 between two processes, and a UDP exchange.
 *
 * The server echoes each byte stream back with every byte incremented; the
 * client sends 200000 bytes in writes of varying size, reads the echo with
 * read and poll, checks it, and then the two trade 20 UDP datagrams.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define TOTAL 200000

static struct sockaddr_in addr(int port) {
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

static int server(int ready_fd) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = addr(7000);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) || listen(ls, 4)) return 10;
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = addr(7001);
    if (bind(u, (struct sockaddr*)&ua, sizeof(ua))) return 11;
    write(ready_fd, "r", 1);
    close(ready_fd);
    int c = accept(ls, 0, 0);
    if (c < 0) return 12;
    unsigned char buf[3000];
    long echoed = 0;
    for (;;) {
        ssize_t n = read(c, buf, sizeof(buf));
        if (n < 0) return 13;
        if (n == 0) break;
        for (ssize_t i = 0; i < n; i++) buf[i]++;
        ssize_t done = 0;
        while (done < n) {
            ssize_t w = write(c, buf + done, (size_t)(n - done));
            if (w < 0) return 14;
            done += w;
        }
        echoed += n;
    }
    close(c);
    for (int k = 0; k < 20; k++) {
        char d[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(u, d, sizeof(d), 0, (struct sockaddr*)&from, &fl);
        if (n <= 0) return 15;
        d[0] = 'R';
        sendto(u, d, (size_t)n, 0, (struct sockaddr*)&from, fl);
    }
    printf("netloop server: echoed %ld bytes, answered 20 datagrams\n", echoed);
    return 0;
}

static int client(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = addr(7000);
    if (connect(s, (struct sockaddr*)&a, sizeof(a))) return 20;
    struct sockaddr_in me;
    socklen_t ml = sizeof(me);
    getsockname(s, (struct sockaddr*)&me, &ml);
    static unsigned char out[TOTAL], back[TOTAL];
    for (int i = 0; i < TOTAL; i++) out[i] = (unsigned char)(i * 31 + 7);
    long sent = 0, got = 0, polls = 0;
    unsigned sizes[] = { 1, 1000, 4096, 7, 30000, 512 };
    for (int k = 0; got < TOTAL; k++) {
        struct pollfd p = { s, (short)(POLLIN | (sent < TOTAL ? POLLOUT : 0)), 0 };
        if (poll(&p, 1, 5000) <= 0) return 21;
        polls++;
        if ((p.revents & POLLOUT) && sent < TOTAL) {
            long n = sizes[k % 6];
            if (n > TOTAL - sent) n = TOTAL - sent;
            ssize_t w = write(s, out + sent, (size_t)n);
            if (w < 0) return 22;
            sent += w;
            if (sent == TOTAL) shutdown(s, SHUT_WR);
        }
        if (p.revents & POLLIN) {
            ssize_t r = read(s, back + got, (size_t)(TOTAL - got));
            if (r < 0) return 23;
            if (r == 0) break;
            got += r;
        }
    }
    long bad = 0;
    for (int i = 0; i < TOTAL; i++) bad += back[i] != (unsigned char)(out[i] + 1);
    close(s);
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = addr(7001);
    int answered = 0;
    for (int k = 0; k < 20; k++) {
        char d[32];
        int n = snprintf(d, sizeof(d), "datagram %d", k);
        sendto(u, d, (size_t)n, 0, (struct sockaddr*)&ua, sizeof(ua));
        char r[64];
        ssize_t m = recv(u, r, sizeof(r), 0);
        if (m == n && r[0] == 'R' && memcmp(r + 1, d + 1, (size_t)n - 1) == 0) answered++;
    }
    printf("netloop client: port %d, sent %ld, got %ld back, %ld wrong, %ld polls; %d of 20 datagrams answered\n",
           ntohs(me.sin_port), sent, got, bad, polls, answered);
    return bad || got != TOTAL || answered != 20 ? 24 : 0;
}

int main(void) {
    int sync[2];
    pipe(sync);
    pid_t srv = fork();
    if (srv == 0) {
        close(sync[0]);
        _exit(server(sync[1]));
    }
    close(sync[1]);
    char c;
    read(sync[0], &c, 1);                      /* the server is listening */
    int rc = client();
    int st;
    waitpid(srv, &st, 0);
    printf("netloop: client %d, server %d\n", rc, WEXITSTATUS(st));
    return rc || WEXITSTATUS(st);
}
