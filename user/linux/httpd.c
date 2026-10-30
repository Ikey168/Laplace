/* A small HTTP/1.0 server for the network gate (#253): epoll, nonblocking
 * accept4, and one response per connection.
 *
 *   httpd [port]   (default 8080)
 *
 * GET /hello       a fixed body
 * GET /count       how many requests this server has answered, and a random
 *                  number and the clock (inputs the replay must reproduce)
 * GET /big         64 KiB of a known pattern (several TCP segments)
 * GET /quit        answers, then the server exits
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static long g_served;

static void reply(int fd, const char* body, size_t len) {
    char head[128];
    int n = snprintf(head, sizeof(head), "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", len);
    write(fd, head, (size_t)n);
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, body + done, len - done);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return;
        }
        done += (size_t)w;
    }
}

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) || listen(ls, 8)) {
        perror("httpd: bind/listen");
        return 1;
    }
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = ls };
    epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev);
    printf("httpd: listening on port %d\n", port);
    fflush(stdout);
    static char big[65536];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)('a' + (i * 7) % 26);
    for (;;) {
        struct epoll_event got[8];
        int n = epoll_wait(ep, got, 8, -1);
        for (int k = 0; k < n; k++) {
            if (got[k].data.fd == ls) {
                struct sockaddr_in peer;
                socklen_t pl = sizeof(peer);
                int c = accept4(ls, (struct sockaddr*)&peer, &pl, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (c < 0) continue;
                struct epoll_event cev = { .events = EPOLLIN, .data.fd = c };
                epoll_ctl(ep, EPOLL_CTL_ADD, c, &cev);
                continue;
            }
            int c = got[k].data.fd;
            char req[2048];
            ssize_t r = read(c, req, sizeof(req) - 1);
            if (r < 0 && errno == EAGAIN) continue;
            epoll_ctl(ep, EPOLL_CTL_DEL, c, 0);
            if (r <= 0) {
                close(c);
                continue;
            }
            req[r] = 0;
            /* The request may arrive in pieces: wait for the blank line. */
            while (!strstr(req, "\r\n\r\n") && r < (ssize_t)sizeof(req) - 1) {
                ssize_t more = read(c, req + r, sizeof(req) - 1 - (size_t)r);
                if (more < 0 && errno == EAGAIN) {
                    struct epoll_event one_ev;
                    int e1 = epoll_create1(0);
                    struct epoll_event w = { .events = EPOLLIN, .data.fd = c };
                    epoll_ctl(e1, EPOLL_CTL_ADD, c, &w);
                    epoll_wait(e1, &one_ev, 1, 5000);
                    close(e1);
                    continue;
                }
                if (more <= 0) break;
                r += more;
                req[r] = 0;
            }
            g_served++;
            int quit = strncmp(req, "GET /quit", 9) == 0;
            if (strncmp(req, "GET /count", 10) == 0) {
                unsigned v;
                getrandom(&v, sizeof(v), 0);
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                char body[128];
                int bl = snprintf(body, sizeof(body), "served %ld random %u clock %ld\n", g_served, v % 1000,
                                  (long)ts.tv_sec);
                reply(c, body, (size_t)bl);
            } else if (strncmp(req, "GET /big", 8) == 0) {
                reply(c, big, sizeof(big));
            } else {
                reply(c, "hello from Laplace\n", 19);
            }
            shutdown(c, SHUT_WR);
            close(c);
            printf("httpd: request %ld: %.*s\n", g_served, (int)(strcspn(req, "\r\n")), req);
            fflush(stdout);
            if (quit) {
                printf("httpd: %ld requests, exiting\n", g_served);
                return 0;
            }
        }
    }
}
