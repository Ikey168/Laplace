/* Laplace core: an Intel 8254x (e1000) network card (#253).
 * See include/core/e1000.h. */

#include "core/e1000.h"
#include "core/pci.h"
#include "core/mm.h"
#include "core/console.h"

void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_SLU   (1u << 6)
#define CTRL_RST   (1u << 26)
#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)

#define RX_N   32
#define TX_N   32
#define BUF_SZ 2048

typedef struct {
    uint64_t addr;
    uint16_t len;
    uint16_t csum;
    uint8_t  status;           /* DD 0x01, EOP 0x02 */
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t len;
    uint8_t  cso;
    uint8_t  cmd;              /* EOP 0x01, IFCS 0x02, RS 0x08 */
    uint8_t  status;           /* DD 0x01 */
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) tx_desc_t;

static volatile uint8_t* g_regs;
static rx_desc_t*        g_rx;
static tx_desc_t*        g_tx;
static uint64_t          g_rx_buf[RX_N], g_tx_buf[TX_N];   /* physical */
static uint32_t          g_rx_next, g_tx_next;
static uint8_t           g_mac[6];

static uint32_t rd(uint32_t off) { return *(volatile uint32_t*)(g_regs + off); }
static void wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_regs + off) = v; }

bool e1000_present(void) { return g_regs != 0; }
void e1000_mac(uint8_t mac[6]) { memcpy(mac, g_mac, 6); }

static bool is_e1000(uint32_t id) {
    if ((id & 0xFFFF) != 0x8086) return false;
    uint32_t dev = id >> 16;
    return dev == 0x100E || dev == 0x100F || dev == 0x10D3 || dev == 0x1004 || dev == 0x100C;
}

/* Two 2 KiB buffers per frame of memory. */
static bool alloc_bufs(uint64_t* bufs, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 2) {
        uint64_t f = pmm_alloc();
        if (!f) return false;
        memset(P2V(f), 0, PAGE_SIZE);
        bufs[i] = f;
        if (i + 1 < n) bufs[i + 1] = f + BUF_SZ;
    }
    return true;
}

bool e1000_init(void) {
    for (uint32_t bus = 0; bus < 8; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read(bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF || !is_e1000(id)) continue;
            uint32_t bar0 = pci_read(bus, dev, 0, 0x10);
            if (bar0 & 1) continue;                                /* I/O space: not used */
            uint32_t cmd = pci_read(bus, dev, 0, 4);
            pci_write(bus, dev, 0, 4, (cmd & 0xFFFF0000u) | (cmd & 0xFFFF) | 0x6 | 0x400);
            g_regs = (volatile uint8_t*)mm_mmio(bar0 & ~0xFULL, 0x20000);
            wr(REG_IMC, 0xFFFFFFFFu);                              /* polled: no interrupts */
            wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
            for (volatile int i = 0; i < 100000 && (rd(REG_CTRL) & CTRL_RST); i++) { }
            wr(REG_IMC, 0xFFFFFFFFu);
            wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU);
            uint32_t ral = rd(REG_RAL), rah = rd(REG_RAH);
            for (int i = 0; i < 4; i++) g_mac[i] = (uint8_t)(ral >> (8 * i));
            g_mac[4] = (uint8_t)rah;
            g_mac[5] = (uint8_t)(rah >> 8);
            wr(REG_RAH, rah | (1u << 31));                         /* address valid */
            for (uint32_t i = 0; i < 128; i++) wr(REG_MTA + 4 * i, 0);

            uint64_t rxd = pmm_alloc(), txd = pmm_alloc();
            if (!rxd || !txd || !alloc_bufs(g_rx_buf, RX_N) || !alloc_bufs(g_tx_buf, TX_N)) {
                g_regs = 0;
                return false;
            }
            g_rx = (rx_desc_t*)P2V(rxd);
            g_tx = (tx_desc_t*)P2V(txd);
            memset(g_rx, 0, PAGE_SIZE);
            memset(g_tx, 0, PAGE_SIZE);
            for (uint32_t i = 0; i < RX_N; i++) g_rx[i].addr = g_rx_buf[i];
            for (uint32_t i = 0; i < TX_N; i++) {
                g_tx[i].addr = g_tx_buf[i];
                g_tx[i].status = 0x01;                             /* free */
            }
            wr(REG_RDBAL, (uint32_t)rxd);
            wr(REG_RDBAH, (uint32_t)(rxd >> 32));
            wr(REG_RDLEN, RX_N * sizeof(rx_desc_t));
            wr(REG_RDH, 0);
            wr(REG_RDT, RX_N - 1);
            wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);         /* 2 KiB buffers */
            wr(REG_TDBAL, (uint32_t)txd);
            wr(REG_TDBAH, (uint32_t)(txd >> 32));
            wr(REG_TDLEN, TX_N * sizeof(tx_desc_t));
            wr(REG_TDH, 0);
            wr(REG_TDT, 0);
            wr(REG_TIPG, 10 | (8u << 10) | (6u << 20));
            wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
            g_rx_next = g_tx_next = 0;
            kprintf("boot: network: e1000 %02x:%02x.0 (%04x:%04x), MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
                    bus, dev, id & 0xFFFF, id >> 16, g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4],
                    g_mac[5], (rd(REG_STATUS) & 2) ? "up" : "down");
            return true;
        }
    }
    return false;
}

uint32_t e1000_receive(uint8_t* buf, uint32_t cap) {
    if (!g_regs) return 0;
    for (;;) {
        rx_desc_t* d = &g_rx[g_rx_next];
        if (!(d->status & 0x01)) return 0;
        uint32_t len = d->len;
        bool whole = (d->status & 0x02) && !d->errors && len <= cap;
        if (whole) memcpy(buf, P2V(g_rx_buf[g_rx_next]), len);
        d->status = 0;
        wr(REG_RDT, g_rx_next);                                    /* the buffer is the card's again */
        g_rx_next = (g_rx_next + 1) % RX_N;
        if (whole) return len;                                     /* else: dropped, try the next */
    }
}

void e1000_send(const uint8_t* frame, uint32_t len) {
    if (!g_regs || len > BUF_SZ) return;
    tx_desc_t* d = &g_tx[g_tx_next];
    if (!(d->status & 0x01)) return;                               /* ring full: dropped */
    memcpy(P2V(g_tx_buf[g_tx_next]), frame, len);
    if (len < 60) {
        memset((uint8_t*)P2V(g_tx_buf[g_tx_next]) + len, 0, 60 - len);
        len = 60;
    }
    d->len = (uint16_t)len;
    d->cmd = 0x01 | 0x02 | 0x08;                                   /* EOP, IFCS, RS */
    d->status = 0;
    g_tx_next = (g_tx_next + 1) % TX_N;
    wr(REG_TDT, g_tx_next);
}
