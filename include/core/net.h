/* Laplace core: the network (#253).
 *
 * The card (core/e1000.h) is outside the machine, like the UART: while the
 * machine runs live, the timer tick (and the idle loop) moves received frames
 * into a queue outside the machine. At a kernel entry they are delivered to
 * the machine's network stack and journaled with that step (IOREC_NET); a
 * replay delivers the journaled frames at the same steps and never touches
 * the card. What the stack transmits goes out live and is suppressed in a
 * replay (machine_device_write).
 *
 * The stack is kernel state, in every keyframe: an address (10.0.2.15/24,
 * gateway 10.0.2.2: QEMU's user-mode network), ARP, IPv4, ICMP echo, UDP,
 * and TCP. TCP takes segments in order only and has no retransmission timer
 * (the virtual link does not lose frames; a lost one stalls its connection),
 * and TIME_WAIT is not kept. Connections to 127.0.0.0/8 or to the machine's
 * own address never reach the card: a loopback stream socket is joined to
 * its peer directly, like a pair of pipes, and a loopback datagram goes
 * straight to the bound socket.
 *
 * Sockets are numbered; the files layer (core/files.h) owns the descriptors
 * and copies to and from user memory. Calls that cannot complete now return
 * NET_EBLOCK: the files layer blocks the process and retries the call when
 * the network changes (net_changed).
 */

#ifndef CORE_NET_H
#define CORE_NET_H

#include <stdint.h>
#include <stdbool.h>

#define KS_ID_NET 10

#define NET_EBLOCK (-512)

#define NET_SOCK_STREAM 1
#define NET_SOCK_DGRAM  2

typedef struct {
    uint32_t ip;               /* host byte order */
    uint16_t port;
} net_addr_t;

void net_init(void);

/* ---- The card, outside the machine ---- */
bool net_has_nic(void);
void net_poll_nic(void);               /* live: card -> pending queue; every tick */
bool net_pending(void);
void net_discard_pending(void);        /* frames that arrived while stopped */
/* At step `step`: deliver pending frames (live, journaled) or the journaled
 * ones (replay), as tty_deliver does. True when something was delivered. */
bool net_deliver(uint64_t step, bool idle);
/* Something changed (data, a connection, a close): blocked calls retry. */
bool net_changed(void);

/* ---- Sockets ---- */
int32_t  net_socket(int type);         /* an id, or -errno */
void     net_close(int32_t s);
int64_t  net_bind(int32_t s, net_addr_t a);
int64_t  net_listen(int32_t s, int backlog);
int64_t  net_accept(int32_t s, int32_t* ns, net_addr_t* peer);
int64_t  net_connect(int32_t s, net_addr_t a, bool nonblock);
int64_t  net_send(int32_t s, const uint8_t* buf, uint32_t len, const net_addr_t* to);
int64_t  net_recv(int32_t s, uint8_t* buf, uint32_t cap, net_addr_t* from, bool peek);
int64_t  net_shutdown(int32_t s, int how);
int64_t  net_name(int32_t s, bool peer, net_addr_t* a);
int      net_type(int32_t s);
int      net_take_error(int32_t s);    /* SO_ERROR */
/* poll bits (POLLIN 1, POLLOUT 4, POLLERR 8, POLLHUP 0x10) ready now. */
uint32_t net_ready(int32_t s);
/* A wait on it may end by a frame from the card (not loopback only). */
bool     net_external(int32_t s);

#endif /* CORE_NET_H */
