/* Laplace core: an AHCI (SATA) disk for the recording store (#236).
 * See include/core/ahci.h. */

#include "core/ahci.h"
#include "core/cpu.h"
#include "core/mm.h"
#include "core/console.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

/* ---- PCI configuration space (mechanism #1) ---- */

static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint32_t pci_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}

static void pci_write(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

/* ---- HBA registers ---- */

#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_PI    0x0C
#define GHC_AE    (1u << 31)

#define PORT_BASE(p) (0x100 + (p) * 0x80)
#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0C
#define PX_IS    0x10
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SERR  0x30
#define PX_CI    0x38

#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define TFD_ERR  (1u << 0)
#define TFD_BSY  (1u << 7)
#define TFD_DRQ  (1u << 3)
#define IS_TFES  (1u << 30)
#define SIG_SATA 0x00000101u

#define ATA_IDENTIFY       0xEC
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA

#define BOUNCE_FRAMES 16                     /* 64 KiB: 128 sectors per command */
#define MAX_SECTORS   (BOUNCE_FRAMES * 8)

typedef struct {
    volatile uint8_t* abar;
    uint32_t port;
    uint64_t clb, fb, ctba;                  /* physical: command list, FIS area, command table */
    uint64_t bounce[BOUNCE_FRAMES];
    uint64_t sectors;
} ahci_t;

static ahci_t             g_ahci;
static fat_block_device_t g_dev;

static uint32_t rd(ahci_t* a, uint32_t off) { return *(volatile uint32_t*)(a->abar + off); }
static void wr(ahci_t* a, uint32_t off, uint32_t v) { *(volatile uint32_t*)(a->abar + off) = v; }
static uint32_t prd(ahci_t* a, uint32_t off) { return rd(a, PORT_BASE(a->port) + off); }
static void pwr(ahci_t* a, uint32_t off, uint32_t v) { wr(a, PORT_BASE(a->port) + off, v); }

static bool wait_clear(ahci_t* a, uint32_t off, uint32_t bits) {
    for (uint32_t i = 0; i < 10000000; i++) {
        if (!(prd(a, off) & bits)) return true;
        __asm__ volatile("pause");
    }
    return false;
}

/* One command in slot 0: an H2D register FIS and `nbytes` of data in the
 * bounce frames. */
static bool issue(ahci_t* a, uint8_t command, uint64_t lba, uint32_t count, uint32_t nbytes, bool write) {
    if (!wait_clear(a, PX_TFD, TFD_BSY | TFD_DRQ)) return false;
    uint32_t* hdr = (uint32_t*)P2V(a->clb);
    uint32_t nprd = (nbytes + PAGE_SIZE - 1) / PAGE_SIZE;
    hdr[0] = 5u | (write ? (1u << 6) : 0) | (nprd << 16);     /* CFL 5 dwords, W, PRDTL */
    hdr[1] = 0;                                                /* PRDBC */
    hdr[2] = (uint32_t)a->ctba;
    hdr[3] = (uint32_t)(a->ctba >> 32);
    uint8_t* tbl = (uint8_t*)P2V(a->ctba);
    memset(tbl, 0, 0x80 + 16 * BOUNCE_FRAMES);
    uint8_t* fis = tbl;
    fis[0] = 0x27;                 /* H2D register FIS */
    fis[1] = 0x80;                 /* a command */
    fis[2] = command;
    fis[4] = (uint8_t)lba; fis[5] = (uint8_t)(lba >> 8); fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40;                 /* LBA mode */
    fis[8] = (uint8_t)(lba >> 24); fis[9] = (uint8_t)(lba >> 32); fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count; fis[13] = (uint8_t)(count >> 8);
    uint32_t* prdt = (uint32_t*)(tbl + 0x80);
    for (uint32_t i = 0; i < nprd; i++) {
        uint32_t len = nbytes - i * PAGE_SIZE < PAGE_SIZE ? nbytes - i * PAGE_SIZE : PAGE_SIZE;
        prdt[i * 4 + 0] = (uint32_t)a->bounce[i];
        prdt[i * 4 + 1] = (uint32_t)(a->bounce[i] >> 32);
        prdt[i * 4 + 2] = 0;
        prdt[i * 4 + 3] = len - 1;                             /* byte count - 1 */
    }
    pwr(a, PX_IS, 0xFFFFFFFFu);
    pwr(a, PX_CI, 1);
    for (uint32_t i = 0; i < 100000000; i++) {
        if (!(prd(a, PX_CI) & 1)) break;
        if (prd(a, PX_IS) & IS_TFES) return false;
        __asm__ volatile("pause");
    }
    if (prd(a, PX_CI) & 1) return false;
    return !(prd(a, PX_TFD) & TFD_ERR) && !(prd(a, PX_IS) & IS_TFES);
}

static int dev_read(void* d, uint32_t sector, uint32_t count, void* buffer) {
    ahci_t* a = (ahci_t*)d;
    uint8_t* out = (uint8_t*)buffer;
    while (count) {
        uint32_t n = count < MAX_SECTORS ? count : MAX_SECTORS;
        if ((uint64_t)sector + n > a->sectors) return -1;
        if (!issue(a, ATA_READ_DMA_EXT, sector, n, n * 512, false)) return -1;
        for (uint32_t done = 0; done < n * 512; done += PAGE_SIZE) {
            uint32_t len = n * 512 - done < PAGE_SIZE ? n * 512 - done : PAGE_SIZE;
            memcpy(out + done, P2V(a->bounce[done / PAGE_SIZE]), len);
        }
        out += n * 512;
        sector += n;
        count -= n;
    }
    return 0;
}

static int dev_write(void* d, uint32_t sector, uint32_t count, const void* buffer) {
    ahci_t* a = (ahci_t*)d;
    const uint8_t* in = (const uint8_t*)buffer;
    while (count) {
        uint32_t n = count < MAX_SECTORS ? count : MAX_SECTORS;
        if ((uint64_t)sector + n > a->sectors) return -1;
        for (uint32_t done = 0; done < n * 512; done += PAGE_SIZE) {
            uint32_t len = n * 512 - done < PAGE_SIZE ? n * 512 - done : PAGE_SIZE;
            memcpy(P2V(a->bounce[done / PAGE_SIZE]), in + done, len);
        }
        if (!issue(a, ATA_WRITE_DMA_EXT, sector, n, n * 512, true)) return -1;
        in += n * 512;
        sector += n;
        count -= n;
    }
    /* Durable before the store goes on (a keyframe is committed after it). */
    return issue(a, ATA_FLUSH_EXT, 0, 0, 0, false) ? 0 : -1;
}

/* Stop the port, give it a command list and a FIS area, start it again. */
static bool port_start(ahci_t* a) {
    pwr(a, PX_CMD, prd(a, PX_CMD) & ~(CMD_ST | CMD_FRE));
    if (!wait_clear(a, PX_CMD, CMD_CR | CMD_FR)) return false;
    a->clb = pmm_alloc();
    a->fb = pmm_alloc();
    a->ctba = pmm_alloc();
    for (int i = 0; i < BOUNCE_FRAMES; i++) a->bounce[i] = pmm_alloc();
    if (!a->clb || !a->fb || !a->ctba || !a->bounce[BOUNCE_FRAMES - 1]) return false;
    pwr(a, PX_CLB, (uint32_t)a->clb);
    pwr(a, PX_CLBU, (uint32_t)(a->clb >> 32));
    pwr(a, PX_FB, (uint32_t)a->fb);
    pwr(a, PX_FBU, (uint32_t)(a->fb >> 32));
    pwr(a, PX_SERR, 0xFFFFFFFFu);
    pwr(a, PX_IS, 0xFFFFFFFFu);
    pwr(a, PX_CMD, prd(a, PX_CMD) | CMD_FRE);
    pwr(a, PX_CMD, prd(a, PX_CMD) | CMD_ST);
    return true;
}

fat_block_device_t* ahci_boot_bind(void) {
    for (uint32_t bus = 0; bus < 8; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            for (uint32_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (fn == 0) break;
                    continue;
                }
                uint32_t cls = pci_read(bus, dev, fn, 8) >> 8;   /* class, subclass, prog-if */
                if (cls != 0x010601) continue;
                uint64_t bar = pci_read(bus, dev, fn, 0x24) & ~0xFULL;   /* BAR5: ABAR */
                if (!bar) continue;
                /* Memory space and bus mastering on, legacy interrupts off. */
                uint32_t cmd = pci_read(bus, dev, fn, 4);
                pci_write(bus, dev, fn, 4, (cmd & 0xFFFF0000u) | (cmd & 0xFFFF) | 0x6 | 0x400);
                ahci_t* a = &g_ahci;
                a->abar = (volatile uint8_t*)mm_mmio(bar, 0x1100);
                wr(a, HBA_GHC, rd(a, HBA_GHC) | GHC_AE);
                uint32_t pi = rd(a, HBA_PI);
                for (uint32_t p = 0; p < 32; p++) {
                    if (!(pi & (1u << p))) continue;
                    a->port = p;
                    if ((prd(a, PX_SSTS) & 0xF) != 3 || prd(a, PX_SIG) != SIG_SATA) continue;
                    if (!port_start(a)) continue;
                    if (!issue(a, ATA_IDENTIFY, 0, 0, 512, false)) continue;
                    const uint16_t* w = (const uint16_t*)P2V(a->bounce[0]);
                    a->sectors = (uint64_t)w[100] | (uint64_t)w[101] << 16 | (uint64_t)w[102] << 32 |
                                 (uint64_t)w[103] << 48;
                    if (!a->sectors) a->sectors = (uint64_t)w[60] | (uint64_t)w[61] << 16;
                    if (a->sectors > 0xFFFFFFFFu) a->sectors = 0xFFFFFFFFu;   /* 32-bit sector numbers */
                    kprintf("boot: AHCI %02x:%02x.%u (%04x:%04x), port %u: a SATA disk of %lu sectors\n",
                            bus, dev, fn, id & 0xFFFF, id >> 16, p, a->sectors);
                    g_dev.read_sectors = dev_read;
                    g_dev.write_sectors = dev_write;
                    g_dev.sector_size = 512;
                    g_dev.total_sectors = (uint32_t)a->sectors;
                    g_dev.private_data = a;
                    return &g_dev;
                }
            }
        }
    }
    return 0;
}
