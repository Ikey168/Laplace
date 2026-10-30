/* Laplace core: the network (#253). See include/core/net.h. */

#include "core/net.h"
#include "core/e1000.h"
#include "core/kio.h"
#include "core/kstate.h"
#include "core/machine.h"
#include "core/linux.h"
#include "core/mm.h"
#include "core/console.h"
#include "iorec.h"
#include "divergence.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);

#define OUR_IP   0x0A00020Fu         /* 10.0.2.15 */
#define GW_IP    0x0A000202u         /* 10.0.2.2 */
#define NETMASK  0xFFFFFF00u
#define MTU      1500
#define MSS      1460
#define FRAME_MAX 1514

static inline int64_t err(int e) { return -(int64_t)e; }
#define E_ADDRINUSE    98
#define E_ADDRNOTAVAIL 99
#define E_NETUNREACH   101
#define E_CONNRESET    104
#define E_ISCONN       106
#define E_NOTCONN      107
#define E_CONNREFUSED  111
#define E_ALREADY      114
#define E_INPROGRESS   115
#define E_OPNOTSUPP    95
#define E_DESTADDRREQ  89
#define E_MSGSIZE      90

/* ---- Outside the machine: frames the card received, not yet delivered ---- */

#define RXQ_N 64

static uint8_t  g_rxq[RXQ_N][FRAME_MAX];
static uint16_t g_rxq_len[RXQ_N];
static uint32_t g_rxq_n;
static bool     g_nic;
static uint8_t  g_mac[6];

bool net_has_nic(void) { return g_nic; }

void net_poll_nic(void) {
    while (g_nic && g_rxq_n < RXQ_N) {
        uint32_t n = e1000_receive(g_rxq[g_rxq_n], FRAME_MAX);
        if (!n) break;
        g_rxq_len[g_rxq_n++] = (uint16_t)n;
    }
}

bool net_pending(void) { return g_rxq_n != 0; }

void net_discard_pending(void) {
    uint8_t tmp[FRAME_MAX];
    while (g_nic && e1000_receive(tmp, sizeof(tmp))) { }
    g_rxq_n = 0;
}

/* ---- Inside the machine ---- */

enum { T_CLOSED = 0, T_LISTEN, T_SYN_SENT, T_SYN_RCVD, T_ESTABLISHED, T_FIN_WAIT1, T_FIN_WAIT2,
       T_CLOSE_WAIT, T_LAST_ACK, T_CLOSING };

#define NSOCK    64
#define RBUF     65536
#define SBUF     65536
#define QMAX     16

typedef struct {
    bool       used;
    bool       open;             /* a descriptor refers to it (else an orphan finishing) */
    uint8_t    type;             /* NET_SOCK_* */
    uint8_t    state;            /* T_* (streams) */
    net_addr_t local, remote;
    bool       bound, connected;
    bool       loop;             /* a loopback stream: joined to `peer` */
    int32_t    peer;
    int32_t    listener;         /* waiting in that listener's queue */
    uint32_t   backlog, nq;
    int32_t    q[QMAX];
    uint8_t*   rbuf;             /* received bytes (or datagrams), a ring */
    uint32_t   rhead, rlen;
    uint8_t*   sbuf;             /* TCP over the card: unacknowledged and unsent bytes */
    uint32_t   slen;
    uint32_t   iss, snd_una, snd_nxt, rcv_nxt, peer_wnd;
    bool       rd_shut, wr_shut, fin_rcvd, fin_sent, connect_pending, need_arp;
    int        err;
} sock_t;

static sock_t   g_s[NSOCK];
static uint32_t g_isn = 0x1000;
static uint16_t g_ephemeral = 49152;
static uint16_t g_ip_id = 1;
static bool     g_changed;

#define ARP_N 8
typedef struct { uint32_t ip; uint8_t mac[6]; } arp_t;
static arp_t    g_arp[ARP_N];
static uint32_t g_narp;

bool net_changed(void) {
    bool c = g_changed;
    g_changed = false;
    return c;
}

static bool is_loop_ip(uint32_t ip) { return (ip >> 24) == 127 || ip == OUR_IP; }

/* ---- Rings ---- */

static uint32_t ring_put(uint8_t* b, uint32_t cap, uint32_t head, uint32_t* len, const uint8_t* src, uint32_t n) {
    uint32_t space = cap - *len;
    if (n > space) n = space;
    uint32_t tail = (head + *len) % cap;
    uint32_t first = cap - tail < n ? cap - tail : n;
    memcpy(b + tail, src, first);
    memcpy(b, src + first, n - first);
    *len += n;
    return n;
}

static void ring_peek(const uint8_t* b, uint32_t cap, uint32_t head, uint32_t off, uint8_t* dst, uint32_t n) {
    uint32_t at = (head + off) % cap;
    uint32_t first = cap - at < n ? cap - at : n;
    memcpy(dst, b + at, first);
    memcpy(dst + first, b, n - first);
}

static bool ensure_bufs(sock_t* s) {
    if (!s->rbuf) s->rbuf = (uint8_t*)kmalloc(RBUF);
    if (s->type == NET_SOCK_STREAM && !s->sbuf) s->sbuf = (uint8_t*)kmalloc(SBUF);
    return s->rbuf && (s->type != NET_SOCK_STREAM || s->sbuf);
}

static int32_t sock_alloc(int type) {
    for (int32_t i = 0; i < NSOCK; i++) {
        if (g_s[i].used) continue;
        sock_t* s = &g_s[i];
        memset(s, 0, sizeof(*s));
        s->used = s->open = true;
        s->type = (uint8_t)type;
        s->peer = s->listener = -1;
        if (!ensure_bufs(s)) {
            kfree(s->rbuf);
            kfree(s->sbuf);
            memset(s, 0, sizeof(*s));
            return -1;
        }
        return i;
    }
    return -1;
}

static void sock_free(int32_t i) {
    sock_t* s = &g_s[i];
    kfree(s->rbuf);
    kfree(s->sbuf);
    memset(s, 0, sizeof(*s));
    g_changed = true;
}

/* ---- Frames out ---- */

static uint16_t csum_add(uint32_t sum, const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i + 1 < n; i += 2) sum += (uint32_t)p[i] << 8 | p[i + 1];
    if (n & 1) sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

static bool arp_lookup(uint32_t ip, uint8_t mac[6]) {
    for (uint32_t i = 0; i < g_narp; i++) {
        if (g_arp[i].ip == ip) {
            memcpy(mac, g_arp[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_learn(uint32_t ip, const uint8_t mac[6]) {
    for (uint32_t i = 0; i < g_narp; i++) {
        if (g_arp[i].ip == ip) {
            memcpy(g_arp[i].mac, mac, 6);
            return;
        }
    }
    uint32_t at = g_narp < ARP_N ? g_narp++ : ip % ARP_N;
    g_arp[at].ip = ip;
    memcpy(g_arp[at].mac, mac, 6);
}

static void xmit(const uint8_t* frame, uint32_t len) {
    machine_device_write(MACHINE_DEV_NET, frame, len);
}

static void arp_send(uint16_t op, const uint8_t* tha, uint32_t tpa) {
    uint8_t f[42];
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    memcpy(f, op == 1 ? bcast : tha, 6);
    memcpy(f + 6, g_mac, 6);
    put16(f + 12, 0x0806);
    put16(f + 14, 1);
    put16(f + 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    put16(f + 20, op);
    memcpy(f + 22, g_mac, 6);
    put32(f + 28, OUR_IP);
    if (op == 1) memset(f + 32, 0, 6); else memcpy(f + 32, tha, 6);
    put32(f + 38, tpa);
    xmit(f, sizeof(f));
}

static uint32_t next_hop(uint32_t ip) { return (ip & NETMASK) == (OUR_IP & NETMASK) ? ip : GW_IP; }

/* An IPv4 packet with `len` bytes of payload already at f + 34. False when
 * the next hop's address is unknown (an ARP request went out). */
static bool ip_send(uint8_t* f, uint32_t dst, uint8_t proto, uint32_t len) {
    uint8_t mac[6];
    if (!arp_lookup(next_hop(dst), mac)) {
        arp_send(1, 0, next_hop(dst));
        return false;
    }
    memcpy(f, mac, 6);
    memcpy(f + 6, g_mac, 6);
    put16(f + 12, 0x0800);
    uint8_t* ip = f + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (uint16_t)(20 + len));
    put16(ip + 4, g_ip_id++);
    put16(ip + 6, 0x4000);                     /* don't fragment */
    ip[8] = 64;
    ip[9] = proto;
    put16(ip + 10, 0);
    put32(ip + 12, OUR_IP);
    put32(ip + 16, dst);
    put16(ip + 10, (uint16_t)~csum_add(0, ip, 20));
    xmit(f, 34 + len);
    return true;
}

static uint16_t l4_csum(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t* p, uint32_t len) {
    uint8_t ph[12];
    put32(ph, src);
    put32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = proto;
    put16(ph + 10, (uint16_t)len);
    uint32_t sum = csum_add(0, ph, 12);
    return (uint16_t)~csum_add(sum, p, len);
}

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

static uint16_t rcv_window(const sock_t* s) {
    uint32_t w = RBUF - s->rlen;
    return (uint16_t)(w > 65535 ? 65535 : w);
}

static bool tcp_segment(sock_t* s, uint8_t flags, uint32_t seq, const uint8_t* data, uint32_t n) {
    uint8_t f[FRAME_MAX];
    uint8_t* t = f + 34;
    uint32_t hl = (flags & F_SYN) ? 24 : 20;
    put16(t, s->local.port);
    put16(t + 2, s->remote.port);
    put32(t + 4, seq);
    put32(t + 8, (flags & F_ACK) ? s->rcv_nxt : 0);
    t[12] = (uint8_t)((hl / 4) << 4);
    t[13] = flags;
    put16(t + 14, rcv_window(s));
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (flags & F_SYN) {
        t[20] = 2; t[21] = 4;
        put16(t + 22, MSS);
    }
    if (n) memcpy(t + hl, data, n);
    put16(t + 16, l4_csum(OUR_IP, s->remote.ip, 6, t, hl + n));
    return ip_send(f, s->remote.ip, 6, hl + n);
}

static void tcp_reset_to(const uint8_t* ip, const uint8_t* t, uint32_t datalen) {
    /* A RST for a segment no socket wants. */
    sock_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.local.port = get16(t + 2);
    tmp.remote.port = get16(t);
    tmp.remote.ip = get32(ip + 12);
    uint8_t fl = t[13];
    if (fl & F_RST) return;
    if (fl & F_ACK) {
        tcp_segment(&tmp, F_RST, get32(t + 8), 0, 0);
    } else {
        tmp.rcv_nxt = get32(t + 4) + datalen + ((fl & F_SYN) ? 1 : 0) + ((fl & F_FIN) ? 1 : 0);
        tcp_segment(&tmp, F_RST | F_ACK, 0, 0, 0);
    }
}

/* Send what the window allows, then a FIN once everything is out. */
static void tcp_output(sock_t* s) {
    if (s->state < T_ESTABLISHED || s->need_arp) return;
    uint8_t seg[MSS];
    for (;;) {
        uint32_t inflight = s->snd_nxt - s->snd_una - (s->fin_sent ? 1 : 0);
        uint32_t unsent = s->slen > inflight ? s->slen - inflight : 0;
        uint32_t wnd = s->peer_wnd > inflight ? s->peer_wnd - inflight : 0;
        uint32_t n = unsent < wnd ? unsent : wnd;
        if (n > MSS) n = MSS;
        if (!n || s->fin_sent) break;
        ring_peek(s->sbuf, SBUF, 0, inflight, seg, n);
        if (!tcp_segment(s, F_ACK | F_PSH, s->snd_nxt, seg, n)) return;
        s->snd_nxt += n;
    }
    uint32_t inflight = s->snd_nxt - s->snd_una;
    if (s->wr_shut && !s->fin_sent && inflight == s->slen &&
        (s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT)) {
        if (tcp_segment(s, F_FIN | F_ACK, s->snd_nxt, 0, 0)) {
            s->snd_nxt++;
            s->fin_sent = true;
            s->state = s->state == T_ESTABLISHED ? T_FIN_WAIT1 : T_LAST_ACK;
        }
    }
}

static void finish(int32_t i) {
    /* A connection is over: an orphan goes away, an open socket stays closed. */
    sock_t* s = &g_s[i];
    s->state = T_CLOSED;
    g_changed = true;
    if (!s->open) sock_free(i);
}

/* ---- Frames in ---- */

static int32_t find_tcp(uint32_t rip, uint16_t rport, uint16_t lport) {
    int32_t listen = -1;
    for (int32_t i = 0; i < NSOCK; i++) {
        sock_t* s = &g_s[i];
        if (!s->used || s->type != NET_SOCK_STREAM || s->loop || s->local.port != lport) continue;
        if (s->state == T_LISTEN) {
            listen = i;
            continue;
        }
        if (s->state != T_CLOSED && s->remote.ip == rip && s->remote.port == rport) return i;
    }
    return listen;
}

static void tcp_input(const uint8_t* ip, const uint8_t* t, uint32_t len) {
    if (len < 20) return;
    uint32_t hl = (uint32_t)(t[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    uint32_t rip = get32(ip + 12);
    uint16_t rport = get16(t), lport = get16(t + 2);
    uint32_t seq = get32(t + 4), ack = get32(t + 8);
    uint8_t fl = t[13];
    uint16_t wnd = get16(t + 14);
    const uint8_t* data = t + hl;
    uint32_t n = len - hl;
    int32_t i = find_tcp(rip, rport, lport);
    if (i < 0) {
        tcp_reset_to(ip, t, n);
        return;
    }
    sock_t* s = &g_s[i];
    if (s->state == T_LISTEN) {
        if (!(fl & F_SYN) || (fl & (F_ACK | F_RST))) {
            if (!(fl & F_RST)) tcp_reset_to(ip, t, n);
            return;
        }
        if (s->nq >= s->backlog || s->nq >= QMAX) return;       /* the peer retries */
        int32_t c = sock_alloc(NET_SOCK_STREAM);
        if (c < 0) return;
        sock_t* k = &g_s[c];
        k->open = false;                                         /* until accept() */
        k->listener = i;
        k->local.ip = OUR_IP;
        k->local.port = lport;
        k->remote.ip = rip;
        k->remote.port = rport;
        k->bound = k->connected = true;
        k->iss = g_isn;
        g_isn += 64000;
        k->snd_una = k->iss;
        k->snd_nxt = k->iss + 1;
        k->rcv_nxt = seq + 1;
        k->peer_wnd = wnd;
        k->state = T_SYN_RCVD;
        s->q[s->nq++] = c;
        tcp_segment(k, F_SYN | F_ACK, k->iss, 0, 0);
        return;
    }
    if (fl & F_RST) {
        s->err = s->state == T_SYN_SENT ? E_CONNREFUSED : E_CONNRESET;
        s->fin_rcvd = true;
        finish(i);
        return;
    }
    if (s->state == T_SYN_SENT) {
        if ((fl & (F_SYN | F_ACK)) != (F_SYN | F_ACK) || ack != s->iss + 1) return;
        s->rcv_nxt = seq + 1;
        s->snd_una = ack;
        s->peer_wnd = wnd;
        s->state = T_ESTABLISHED;
        s->connected = true;
        g_changed = true;
        tcp_segment(s, F_ACK, s->snd_nxt, 0, 0);
        tcp_output(s);
        return;
    }
    if (fl & F_SYN) {                                           /* a repeated SYN: answer again */
        if (s->state == T_SYN_RCVD) tcp_segment(s, F_SYN | F_ACK, s->iss, 0, 0);
        return;
    }
    /* ACK. */
    if (fl & F_ACK) {
        if (s->state == T_SYN_RCVD && ack == s->iss + 1) {
            s->state = T_ESTABLISHED;
            s->snd_una = ack;
            g_changed = true;                                    /* the listener can accept */
        }
        uint32_t acked = ack - s->snd_una;
        if (acked && acked <= s->snd_nxt - s->snd_una) {
            uint32_t data_acked = acked;
            if (s->fin_sent && ack == s->snd_nxt) data_acked--;  /* our FIN */
            if (data_acked > s->slen) data_acked = s->slen;
            memmove(s->sbuf, s->sbuf + data_acked, s->slen - data_acked);
            s->slen -= data_acked;
            s->snd_una = ack;
            g_changed = true;
            if (s->fin_sent && ack == s->snd_nxt) {
                if (s->state == T_FIN_WAIT1) s->state = T_FIN_WAIT2;
                else if (s->state == T_CLOSING || s->state == T_LAST_ACK) {
                    finish(i);
                    return;
                }
            }
        }
        s->peer_wnd = wnd;
    }
    /* Data and FIN, in order only. */
    bool ack_needed = false;
    if (n || (fl & F_FIN)) {
        if (seq != s->rcv_nxt) {
            tcp_segment(s, F_ACK, s->snd_nxt, 0, 0);               /* out of order: say where we are */
            return;
        }
        if (n && !s->fin_rcvd) {
            uint32_t took = s->rd_shut ? n : ring_put(s->rbuf, RBUF, s->rhead, &s->rlen, data, n);
            s->rcv_nxt += took;
            ack_needed = true;
            g_changed = true;
            if (took < n) fl &= (uint8_t)~F_FIN;                  /* the FIN comes again */
        }
        if (fl & F_FIN) {
            s->rcv_nxt++;
            s->fin_rcvd = true;
            ack_needed = true;
            g_changed = true;
            if (s->state == T_ESTABLISHED || s->state == T_SYN_RCVD) s->state = T_CLOSE_WAIT;
            else if (s->state == T_FIN_WAIT1) s->state = T_CLOSING;
            else if (s->state == T_FIN_WAIT2) {
                tcp_segment(s, F_ACK, s->snd_nxt, 0, 0);
                finish(i);                                       /* no TIME_WAIT */
                return;
            }
        }
    }
    if (ack_needed) tcp_segment(s, F_ACK, s->snd_nxt, 0, 0);
    tcp_output(s);
}

/* A datagram for a bound socket: [u32 len][u32 ip][u16 port] then the bytes. */
static bool dgram_queue(sock_t* s, uint32_t ip, uint16_t port, const uint8_t* data, uint32_t n) {
    uint8_t h[10];
    if (s->rlen + 10 + n > RBUF) return false;
    put32(h, n);
    put32(h + 4, ip);
    put16(h + 8, port);
    ring_put(s->rbuf, RBUF, s->rhead, &s->rlen, h, 10);
    ring_put(s->rbuf, RBUF, s->rhead, &s->rlen, data, n);
    g_changed = true;
    return true;
}

static int32_t find_udp(uint32_t dst, uint16_t port) {
    for (int32_t i = 0; i < NSOCK; i++) {
        sock_t* s = &g_s[i];
        if (s->used && s->type == NET_SOCK_DGRAM && s->bound && s->local.port == port &&
            (s->local.ip == 0 || s->local.ip == dst || (is_loop_ip(dst) && is_loop_ip(s->local.ip)))) {
            return i;
        }
    }
    return -1;
}

static void udp_input(const uint8_t* ip, const uint8_t* u, uint32_t len) {
    if (len < 8) return;
    uint32_t n = get16(u + 4);
    if (n < 8 || n > len) return;
    int32_t i = find_udp(get32(ip + 16), get16(u + 2));
    if (i >= 0) dgram_queue(&g_s[i], get32(ip + 12), get16(u), u + 8, n - 8);
}

static void icmp_input(const uint8_t* ip, const uint8_t* c, uint32_t len) {
    if (len < 8 || c[0] != 8) return;                        /* echo request */
    uint8_t f[FRAME_MAX];
    if (len > FRAME_MAX - 34) return;
    memcpy(f + 34, c, len);
    f[34] = 0;
    put16(f + 36, 0);
    put16(f + 36, (uint16_t)~csum_add(0, f + 34, len));
    ip_send(f, get32(ip + 12), 1, len);
}

static void frame_input(const uint8_t* f, uint32_t len) {
    if (len < 14) return;
    uint16_t type = get16(f + 12);
    if (type == 0x0806 && len >= 42) {
        uint16_t op = get16(f + 20);
        uint32_t spa = get32(f + 28), tpa = get32(f + 38);
        arp_learn(spa, f + 22);
        if (op == 1 && tpa == OUR_IP) arp_send(2, f + 22, spa);
        /* Connections that waited for this address send their SYN now. */
        for (int32_t i = 0; i < NSOCK; i++) {
            sock_t* s = &g_s[i];
            if (s->used && s->need_arp && next_hop(s->remote.ip) == spa) {
                s->need_arp = false;
                tcp_segment(s, F_SYN, s->iss, 0, 0);
            }
        }
        return;
    }
    if (type != 0x0800 || len < 34) return;
    const uint8_t* ip = f + 14;
    uint32_t ihl = (uint32_t)(ip[0] & 0xF) * 4;
    uint32_t tot = get16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || tot < ihl || 14 + tot > len) return;
    if (get16(ip + 6) & 0x3FFF) return;                      /* fragments: not handled */
    uint32_t dst = get32(ip + 16);
    if (dst != OUR_IP && dst != 0xFFFFFFFFu) return;
    arp_learn(get32(ip + 12) == next_hop(get32(ip + 12)) ? get32(ip + 12) : GW_IP, f + 6);
    const uint8_t* p = ip + ihl;
    uint32_t plen = tot - ihl;
    switch (ip[9]) {
    case 1:  icmp_input(ip, p, plen); break;
    case 6:  tcp_input(ip, p, plen); break;
    case 17: udp_input(ip, p, plen); break;
    default: break;
    }
}

#define IDLE_FLAG (1ULL << 63)

bool net_deliver(uint64_t step, bool idle) {
    static uint8_t buf[FRAME_MAX];
    if (machine_mode() == MACHINE_LIVE) {
        if (!g_rxq_n) return false;
        for (uint32_t k = 0; k < g_rxq_n; k++) {
            kio_record(IOREC_NET, step, g_rxq_len[k] | (idle ? IDLE_FLAG : 0), g_rxq[k], g_rxq_len[k]);
            frame_input(g_rxq[k], g_rxq_len[k]);
        }
        g_rxq_n = 0;
        return true;
    }
    uint64_t args;
    uint32_t len;
    if (!kio_record_peek(IOREC_NET, step, &args) || ((args & IDLE_FLAG) != 0) != idle) return false;
    if (!kio_record_next(IOREC_NET, step, &args, buf, sizeof(buf), &len)) return false;
    frame_input(buf, len);
    return true;
}

/* ---- Sockets ---- */

static sock_t* get(int32_t s) { return s >= 0 && s < NSOCK && g_s[s].used ? &g_s[s] : 0; }

int32_t net_socket(int type) {
    if (type != NET_SOCK_STREAM && type != NET_SOCK_DGRAM) return (int32_t)err(LX_EINVAL);
    int32_t i = sock_alloc(type);
    return i < 0 ? (int32_t)err(LX_ENOMEM) : i;
}

int net_type(int32_t s) { sock_t* k = get(s); return k ? k->type : 0; }

int net_take_error(int32_t s) {
    sock_t* k = get(s);
    if (!k) return 0;
    int e = k->err;
    k->err = 0;
    return e;
}

static bool port_taken(uint8_t type, uint32_t ip, uint16_t port, int32_t self) {
    for (int32_t i = 0; i < NSOCK; i++) {
        sock_t* s = &g_s[i];
        if (i == self || !s->used || s->type != type || !s->bound || s->local.port != port) continue;
        if (s->listener >= 0) continue;                     /* a connection accepted on it */
        if (type == NET_SOCK_STREAM && s->state != T_LISTEN && s->connected) continue;
        if (s->local.ip == 0 || ip == 0 || s->local.ip == ip) return true;
    }
    return false;
}

static void auto_bind(int32_t i) {
    sock_t* s = &g_s[i];
    if (s->bound) return;
    do {
        s->local.port = g_ephemeral++;
        if (g_ephemeral < 49152) g_ephemeral = 49152;
    } while (port_taken(s->type, 0, s->local.port, i));
    s->bound = true;
}

int64_t net_bind(int32_t i, net_addr_t a) {
    sock_t* s = get(i);
    if (!s) return err(LX_EBADF);
    if (s->bound) return err(LX_EINVAL);
    if (a.ip && a.ip != OUR_IP && !is_loop_ip(a.ip)) return err(E_ADDRNOTAVAIL);
    if (a.port && port_taken(s->type, a.ip, a.port, i)) return err(E_ADDRINUSE);
    s->local = a;
    s->bound = true;
    if (!a.port) {
        s->bound = false;
        auto_bind(i);
        s->local.ip = a.ip;
    }
    return 0;
}

int64_t net_listen(int32_t i, int backlog) {
    sock_t* s = get(i);
    if (!s) return err(LX_EBADF);
    if (s->type != NET_SOCK_STREAM) return err(E_OPNOTSUPP);
    if (!s->bound) auto_bind(i);
    s->state = T_LISTEN;
    s->backlog = backlog < 1 ? 1 : backlog > QMAX ? QMAX : (uint32_t)backlog;
    return 0;
}

int64_t net_accept(int32_t i, int32_t* ns, net_addr_t* peer) {
    sock_t* s = get(i);
    if (!s) return err(LX_EBADF);
    if (s->type != NET_SOCK_STREAM || s->state != T_LISTEN) return err(LX_EINVAL);
    for (uint32_t k = 0; k < s->nq; k++) {
        sock_t* c = &g_s[s->q[k]];
        if (c->state == T_SYN_RCVD) continue;                /* not yet established */
        int32_t id = s->q[k];
        memmove(&s->q[k], &s->q[k + 1], (s->nq - k - 1) * sizeof(int32_t));
        s->nq--;
        c->open = true;
        c->listener = -1;
        *ns = id;
        *peer = c->remote;
        return 0;
    }
    return NET_EBLOCK;
}

static int32_t find_loop_listener(uint16_t port) {
    for (int32_t i = 0; i < NSOCK; i++) {
        sock_t* s = &g_s[i];
        if (s->used && s->type == NET_SOCK_STREAM && s->state == T_LISTEN && s->local.port == port &&
            (s->local.ip == 0 || is_loop_ip(s->local.ip))) {
            return i;
        }
    }
    return -1;
}

int64_t net_connect(int32_t i, net_addr_t a, bool nonblock) {
    sock_t* s = get(i);
    if (!s) return err(LX_EBADF);
    if (s->type == NET_SOCK_DGRAM) {
        if (!s->bound) auto_bind(i);
        s->remote = a;
        s->connected = a.ip != 0 || a.port != 0;
        return 0;
    }
    if (s->connect_pending) {                                /* a retry of a blocked connect */
        if (s->state == T_SYN_SENT) return NET_EBLOCK;
        s->connect_pending = false;
        int e = s->err;
        s->err = 0;
        return e ? err(e) : 0;
    }
    if (s->connected || s->state == T_ESTABLISHED) return err(E_ISCONN);
    if (s->state == T_SYN_SENT) return err(E_ALREADY);
    if (s->state != T_CLOSED) return err(LX_EINVAL);
    if (!s->bound) auto_bind(i);
    s->remote = a;
    if (is_loop_ip(a.ip)) {
        /* Loopback: joined to a new socket in the listener's queue. */
        int32_t l = find_loop_listener(a.port);
        if (l < 0 || g_s[l].nq >= g_s[l].backlog) return err(E_CONNREFUSED);
        int32_t c = sock_alloc(NET_SOCK_STREAM);
        if (c < 0) return err(LX_ENOMEM);
        sock_t* k = &g_s[c];
        s = &g_s[i];
        k->open = false;
        k->listener = l;
        k->loop = s->loop = true;
        k->peer = i;
        s->peer = c;
        k->local.ip = a.ip;
        k->local.port = a.port;
        k->remote.ip = s->local.ip ? s->local.ip : 0x7F000001u;
        k->remote.port = s->local.port;
        if (!s->local.ip) s->local.ip = 0x7F000001u;
        k->bound = k->connected = s->connected = true;
        k->state = s->state = T_ESTABLISHED;
        g_s[l].q[g_s[l].nq++] = c;
        g_changed = true;
        return 0;
    }
    if (!g_nic) return err(E_NETUNREACH);
    s->local.ip = OUR_IP;
    s->iss = g_isn;
    g_isn += 64000;
    s->snd_una = s->iss;
    s->snd_nxt = s->iss + 1;
    s->state = T_SYN_SENT;
    s->peer_wnd = 0;
    uint8_t mac[6];
    if (arp_lookup(next_hop(a.ip), mac)) {
        tcp_segment(s, F_SYN, s->iss, 0, 0);
    } else {
        s->need_arp = true;
        arp_send(1, 0, next_hop(a.ip));
    }
    if (nonblock) return err(E_INPROGRESS);
    s->connect_pending = true;
    return NET_EBLOCK;
}

int64_t net_send(int32_t i, const uint8_t* buf, uint32_t len, const net_addr_t* to) {
    sock_t* s = get(i);
    if (!s) return err(LX_EBADF);
    if (s->type == NET_SOCK_DGRAM) {
        net_addr_t d = to ? *to : s->remote;
        if (!to && !s->connected) return err(E_DESTADDRREQ);
        if (len > 65507) return err(E_MSGSIZE);
        if (!s->bound) auto_bind(i);
        if (is_loop_ip(d.ip)) {
            int32_t t = find_udp(d.ip, d.port);
            if (t >= 0) dgram_queue(&g_s[t], s->local.ip ? s->local.ip : 0x7F000001u, s->local.port, buf, len);
            return len;
        }
        if (!g_nic) return err(E_NETUNREACH);
        if (len > MTU - 28) return err(E_MSGSIZE);           /* no fragmentation */
        uint8_t f[FRAME_MAX];
        uint8_t* u = f + 34;
        put16(u, s->local.port);
        put16(u + 2, d.port);
        put16(u + 4, (uint16_t)(8 + len));
        put16(u + 6, 0);
        memcpy(u + 8, buf, len);
        put16(u + 6, l4_csum(OUR_IP, d.ip, 17, u, 8 + len));
        ip_send(f, d.ip, 17, 8 + len);                       /* unknown next hop: lost, as UDP may be */
        return len;
    }
    if (s->wr_shut) return err(LX_EPIPE);
    if (s->err) return err(net_take_error(i));
    if (s->state != T_ESTABLISHED && s->state != T_CLOSE_WAIT) return err(E_NOTCONN);
    if (!len) return 0;
    if (s->loop) {
        sock_t* p = s->peer >= 0 ? &g_s[s->peer] : 0;
        if (!p || p->rd_shut || p->state == T_CLOSED) return err(LX_EPIPE);
        uint32_t took = ring_put(p->rbuf, RBUF, p->rhead, &p->rlen, buf, len);
        if (!took) return NET_EBLOCK;
        g_changed = true;
        return took;
    }
    uint32_t took = SBUF - s->slen < len ? SBUF - s->slen : len;
    if (!took) return NET_EBLOCK;
    memcpy(s->sbuf + s->slen, buf, took);
    s->slen += took;
    tcp_output(s);
    return took;
}

int64_t net_recv(int32_t i, uint8_t* buf, uint32_t cap, net_addr_t* from, bool peek) {
    sock_t* s = get(i);
    if (!s) return err(LX_EBADF);
    if (s->type == NET_SOCK_DGRAM) {
        if (!s->rlen) return NET_EBLOCK;
        uint8_t h[10];
        ring_peek(s->rbuf, RBUF, s->rhead, 0, h, 10);
        uint32_t n = get32(h);
        uint32_t take = n < cap ? n : cap;
        ring_peek(s->rbuf, RBUF, s->rhead, 10, buf, take);
        if (from) { from->ip = get32(h + 4); from->port = get16(h + 8); }
        if (!peek) {
            s->rhead = (s->rhead + 10 + n) % RBUF;
            s->rlen -= 10 + n;
            g_changed = true;
        }
        return take;
    }
    if (from) *from = s->remote;
    if (s->rlen) {
        uint32_t n = s->rlen < cap ? s->rlen : cap;
        bool was_full = rcv_window(s) < MSS;
        ring_peek(s->rbuf, RBUF, s->rhead, 0, buf, n);
        if (!peek) {
            s->rhead = (s->rhead + n) % RBUF;
            s->rlen -= n;
            g_changed = true;
            /* The window opened: tell the peer. */
            if (!s->loop && was_full && s->state >= T_ESTABLISHED) tcp_segment(s, F_ACK, s->snd_nxt, 0, 0);
        }
        return n;
    }
    if (s->err) return err(net_take_error(i));
    if (s->fin_rcvd || s->rd_shut || s->state == T_CLOSED) return 0;
    if (s->loop && (s->peer < 0 || g_s[s->peer].wr_shut)) return 0;
    if (s->state != T_ESTABLISHED && s->state != T_FIN_WAIT1 && s->state != T_FIN_WAIT2) {
        return s->state == T_SYN_SENT ? NET_EBLOCK : err(E_NOTCONN);
    }
    return NET_EBLOCK;
}

int64_t net_shutdown(int32_t i, int how) {
    sock_t* s = get(i);
    if (!s) return err(LX_EBADF);
    if (s->type == NET_SOCK_STREAM && !s->connected) return err(E_NOTCONN);
    if (how == 0 || how == 2) s->rd_shut = true;
    if (how == 1 || how == 2) {
        s->wr_shut = true;
        if (s->loop && s->peer >= 0) g_s[s->peer].fin_rcvd = true;
        else if (s->type == NET_SOCK_STREAM) tcp_output(s);
    }
    g_changed = true;
    return 0;
}

void net_close(int32_t i) {
    sock_t* s = get(i);
    if (!s) return;
    s->open = false;
    g_changed = true;
    if (s->type == NET_SOCK_STREAM && s->state == T_LISTEN) {
        for (uint32_t k = 0; k < s->nq; k++) {
            sock_t* c = &g_s[s->q[k]];
            if (c->loop && c->peer >= 0) {
                g_s[c->peer].peer = -1;
                g_s[c->peer].fin_rcvd = true;
                g_s[c->peer].err = E_CONNRESET;
            } else if (!c->loop && c->state >= T_SYN_RCVD) {
                tcp_segment(c, F_RST | F_ACK, c->snd_nxt, 0, 0);
            }
            sock_free(s->q[k]);
        }
        sock_free(i);
        return;
    }
    if (s->loop) {
        if (s->peer >= 0) {
            g_s[s->peer].peer = -1;
            g_s[s->peer].fin_rcvd = true;
        }
        sock_free(i);
        return;
    }
    if (s->type == NET_SOCK_STREAM && (s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT)) {
        s->wr_shut = true;
        tcp_output(s);                                        /* the rest, then a FIN; an orphan until done */
        if (s->state != T_CLOSED) return;
    }
    if (s->type == NET_SOCK_STREAM && s->state >= T_SYN_SENT && s->state != T_CLOSED &&
        s->state != T_FIN_WAIT1 && s->state != T_FIN_WAIT2 && s->state != T_LAST_ACK && s->state != T_CLOSING) {
        tcp_segment(s, F_RST | F_ACK, s->snd_nxt, 0, 0);
    }
    if (s->state == T_FIN_WAIT1 || s->state == T_FIN_WAIT2 || s->state == T_LAST_ACK || s->state == T_CLOSING) {
        return;                                               /* finishes when the peer answers */
    }
    sock_free(i);
}

int64_t net_name(int32_t i, bool peer, net_addr_t* a) {
    sock_t* s = get(i);
    if (!s) return err(LX_EBADF);
    if (peer && !s->connected) return err(E_NOTCONN);
    *a = peer ? s->remote : s->local;
    return 0;
}

uint32_t net_ready(int32_t i) {
    sock_t* s = get(i);
    if (!s) return 0x20;                                      /* POLLNVAL */
    uint32_t m = 0;
    if (s->type == NET_SOCK_DGRAM) return (s->rlen ? 0x41 : 0) | 0x104;
    if (s->state == T_LISTEN) {
        for (uint32_t k = 0; k < s->nq; k++) {
            if (g_s[s->q[k]].state != T_SYN_RCVD) return 0x41;
        }
        return 0;
    }
    bool peer_gone = s->loop && (s->peer < 0 || g_s[s->peer].wr_shut);
    if (s->rlen || s->fin_rcvd || s->rd_shut || peer_gone || s->err) m |= 0x41;
    if (s->err) m |= 0x08;
    if (s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT) {
        bool space = s->loop ? (s->peer >= 0 && g_s[s->peer].rlen < RBUF) : s->slen < SBUF;
        if (space && !s->wr_shut) m |= 0x104;
    }
    if ((s->fin_rcvd && (s->wr_shut || s->state == T_CLOSED)) || (s->state == T_CLOSED && s->connected)) m |= 0x10;
    return m;
}

bool net_external(int32_t i) {
    sock_t* s = get(i);
    return s && !s->loop && g_nic;
}

/* ---- Keyframes ---- */

/* s's fields in a zeroed copy, without its buffers. A socket's padding
 * (net_addr_t has two bytes, sock_t a few more) holds whatever a struct
 * assignment such as `s->local = a` copied from the stack, which differs
 * between a recording and its replay: only the fields may reach a keyframe
 * or a checksum. */
static void canon(const sock_t* s, sock_t* c) {
    memset(c, 0, sizeof(*c));
    c->used = s->used;
    c->open = s->open;
    c->type = s->type;
    c->state = s->state;
    c->local.ip = s->local.ip;
    c->local.port = s->local.port;
    c->remote.ip = s->remote.ip;
    c->remote.port = s->remote.port;
    c->bound = s->bound;
    c->connected = s->connected;
    c->loop = s->loop;
    c->peer = s->peer;
    c->listener = s->listener;
    c->backlog = s->backlog;
    c->nq = s->nq;
    memcpy(c->q, s->q, sizeof(c->q));
    c->rhead = s->rhead;
    c->rlen = s->rlen;
    c->slen = s->slen;
    c->iss = s->iss;
    c->snd_una = s->snd_una;
    c->snd_nxt = s->snd_nxt;
    c->rcv_nxt = s->rcv_nxt;
    c->peer_wnd = s->peer_wnd;
    c->rd_shut = s->rd_shut;
    c->wr_shut = s->wr_shut;
    c->fin_rcvd = s->fin_rcvd;
    c->fin_sent = s->fin_sent;
    c->connect_pending = s->connect_pending;
    c->need_arp = s->need_arp;
    c->err = s->err;
}

static void save(ksf_writer_t* w, void* ctx) {
    (void)ctx;
    ksf_put_u32(w, g_isn);
    ksf_put_u32(w, g_ephemeral | (uint32_t)g_ip_id << 16);
    ksf_put_u32(w, g_narp);
    ksf_put(w, g_arp, g_narp * sizeof(arp_t));
    uint32_t n = 0;
    for (int32_t i = 0; i < NSOCK; i++) n += g_s[i].used;
    ksf_put_u32(w, n);
    for (int32_t i = 0; i < NSOCK; i++) {
        sock_t* s = &g_s[i];
        if (!s->used) continue;
        ksf_put_u32(w, (uint32_t)i);
        sock_t copy;
        canon(s, &copy);
        ksf_put(w, &copy, sizeof(copy));
        uint8_t tmp[512];
        for (uint32_t off = 0; off < s->rlen; off += sizeof(tmp)) {
            uint32_t k = s->rlen - off < sizeof(tmp) ? s->rlen - off : sizeof(tmp);
            ring_peek(s->rbuf, RBUF, s->rhead, off, tmp, k);
            ksf_put(w, tmp, k);
        }
        if (s->sbuf) ksf_put(w, s->sbuf, s->slen);
    }
}

static int restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    if (version != 1) return -1;
    for (int32_t i = 0; i < NSOCK; i++) {
        if (g_s[i].used) {
            kfree(g_s[i].rbuf);
            kfree(g_s[i].sbuf);
        }
        memset(&g_s[i], 0, sizeof(sock_t));
    }
    g_isn = ksf_get_u32(r);
    uint32_t v = ksf_get_u32(r);
    g_ephemeral = (uint16_t)v;
    g_ip_id = (uint16_t)(v >> 16);
    g_narp = ksf_get_u32(r);
    if (g_narp > ARP_N) return -1;
    ksf_get(r, g_arp, g_narp * sizeof(arp_t));
    uint32_t n = ksf_get_u32(r);
    for (uint32_t k = 0; k < n && r->ok; k++) {
        uint32_t i = ksf_get_u32(r);
        if (i >= NSOCK) return -1;
        sock_t* s = &g_s[i];
        ksf_get(r, s, sizeof(*s));
        s->rbuf = s->sbuf = 0;
        if (s->rlen > RBUF || s->slen > SBUF || !ensure_bufs(s)) return -1;
        s->rhead = 0;
        ksf_get(r, s->rbuf, s->rlen);
        if (s->sbuf) ksf_get(r, s->sbuf, s->slen);
    }
    g_rxq_n = 0;
    return r->ok ? 0 : -1;
}

static uint32_t checksum(void* ctx) {
    (void)ctx;
    uint32_t head[3] = { g_isn, g_ephemeral | (uint32_t)g_ip_id << 16, g_narp };
    uint32_t crc = divergence_checksum(0, head, sizeof(head));
    crc = divergence_checksum(crc, g_arp, g_narp * sizeof(arp_t));
    for (int32_t i = 0; i < NSOCK; i++) {
        sock_t* s = &g_s[i];
        if (!s->used) continue;
        sock_t copy;
        canon(s, &copy);
        copy.rhead = 0;
        crc = divergence_checksum(crc, &i, sizeof(i));
        crc = divergence_checksum(crc, &copy, sizeof(copy));
        uint8_t tmp[512];
        for (uint32_t off = 0; off < s->rlen; off += sizeof(tmp)) {
            uint32_t k = s->rlen - off < sizeof(tmp) ? s->rlen - off : sizeof(tmp);
            ring_peek(s->rbuf, RBUF, s->rhead, off, tmp, k);
            crc = divergence_checksum(crc, tmp, k);
        }
        if (s->sbuf) crc = divergence_checksum(crc, s->sbuf, s->slen);
    }
    return crc;
}

void net_init(void) {
    g_nic = e1000_init();
    if (g_nic) {
        e1000_mac(g_mac);
        kprintf("boot: network: 10.0.2.15/24, gateway 10.0.2.2; received frames are journaled inputs\n");
    }
    static const ksf_subsys_t sub = { KS_ID_NET, "net", 1, save, restore, checksum, 0 };
    if (kstate_register(&sub) != KSF_OK) panic("net: cannot register its keyframe section");
}
