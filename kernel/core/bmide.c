/* Laplace core: bus-master DMA for the IDE disk (#236).
 * See include/core/bmide.h. */

#include "core/bmide.h"
#include "core/cpu.h"
#include "core/mm.h"
#include "core/console.h"
#include "core/pci.h"
#include "checkpoint_ide.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
int   memcmp(const void* a, const void* b, size_t n);

/* Primary channel, compatibility mode. */
#define ATA_DATA     0x1F0
#define ATA_COUNT    0x1F2
#define ATA_LBA0     0x1F3
#define ATA_LBA1     0x1F4
#define ATA_LBA2     0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_CMD      0x1F7           /* status when read */
#define ATA_ALTST    0x3F6
#define ST_ERR       0x01
#define ST_DRQ       0x08
#define ST_DF        0x20
#define ST_BSY       0x80

#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35

/* Bus-master registers of the primary channel, from BAR4. */
#define BM_CMD       0
#define BM_STATUS    2
#define BM_PRDT      4
#define BM_START     0x01
#define BM_TO_MEMORY 0x08            /* the device writes memory: a read */
#define BMS_ACTIVE   0x01
#define BMS_ERROR    0x02
#define BMS_IRQ      0x04

static inline void outl(uint16_t port, uint32_t v) { pci_outl(port, v); }

#define BOUNCE_FRAMES 16             /* 64 KiB: 128 sectors per command */
#define MAX_SECTORS   (BOUNCE_FRAMES * 8)

typedef struct {
    uint16_t bm;
    uint8_t  drive;
    uint64_t prdt;                   /* physical */
    uint64_t bounce[BOUNCE_FRAMES];
    uint32_t sectors;
} bmide_t;

static bmide_t            g_bm;
static fat_block_device_t g_dev;

static bool wait_idle(void) {
    for (uint32_t i = 0; i < 100000000; i++) {
        uint8_t s = inb(ATA_ALTST);
        if (!(s & ST_BSY)) return !(s & (ST_ERR | ST_DF));
        __asm__ volatile("pause");
    }
    return false;
}

/* One DMA command of n sectors, through the first frames of the bounce
 * buffer. */
static bool transfer(bmide_t* b, uint32_t lba, uint32_t n, bool write) {
    uint32_t nbytes = n * 512;
    uint32_t* prd = (uint32_t*)P2V(b->prdt);
    uint32_t nprd = (nbytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < nprd; i++) {
        uint32_t len = nbytes - i * PAGE_SIZE < PAGE_SIZE ? nbytes - i * PAGE_SIZE : PAGE_SIZE;
        prd[i * 2] = (uint32_t)b->bounce[i];
        prd[i * 2 + 1] = len | (i == nprd - 1 ? 0x80000000u : 0);     /* byte count, end of table */
    }
    outb(b->bm + BM_CMD, 0);
    outb(b->bm + BM_STATUS, BMS_ERROR | BMS_IRQ);                     /* write 1 to clear */
    outl(b->bm + BM_PRDT, (uint32_t)b->prdt);
    outb(b->bm + BM_CMD, write ? 0 : BM_TO_MEMORY);
    outb(ATA_DRIVE, 0x40 | (uint8_t)(b->drive << 4));
    if (!wait_idle()) return false;
    outb(ATA_COUNT, (uint8_t)(n >> 8));                               /* LBA48: high bytes first */
    outb(ATA_LBA0, (uint8_t)(lba >> 24));
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_COUNT, (uint8_t)n);
    outb(ATA_LBA0, (uint8_t)lba);
    outb(ATA_LBA1, (uint8_t)(lba >> 8));
    outb(ATA_LBA2, (uint8_t)(lba >> 16));
    outb(ATA_CMD, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT);
    outb(b->bm + BM_CMD, (write ? 0 : BM_TO_MEMORY) | BM_START);
    bool ok = false;
    for (uint32_t i = 0; i < 100000000; i++) {
        uint8_t s = inb(b->bm + BM_STATUS);
        if (s & BMS_ERROR) break;
        if (!(s & BMS_ACTIVE) || (s & BMS_IRQ)) {
            ok = true;
            break;
        }
        __asm__ volatile("pause");
    }
    outb(b->bm + BM_CMD, write ? 0 : BM_TO_MEMORY);
    ok = ok && wait_idle() && !(inb(ATA_CMD) & ST_DRQ);
    outb(b->bm + BM_STATUS, BMS_ERROR | BMS_IRQ);
    return ok;
}

static int dev_read(void* d, uint32_t sector, uint32_t count, void* buffer) {
    bmide_t* b = (bmide_t*)d;
    uint8_t* out = (uint8_t*)buffer;
    while (count) {
        uint32_t n = count < MAX_SECTORS ? count : MAX_SECTORS;
        if ((uint64_t)sector + n > b->sectors) return -1;
        if (!transfer(b, sector, n, false)) return -1;
        for (uint32_t done = 0; done < n * 512; done += PAGE_SIZE) {
            uint32_t len = n * 512 - done < PAGE_SIZE ? n * 512 - done : PAGE_SIZE;
            memcpy(out + done, P2V(b->bounce[done / PAGE_SIZE]), len);
        }
        out += n * 512;
        sector += n;
        count -= n;
    }
    return 0;
}

static int dev_write(void* d, uint32_t sector, uint32_t count, const void* buffer) {
    bmide_t* b = (bmide_t*)d;
    const uint8_t* in = (const uint8_t*)buffer;
    while (count) {
        uint32_t n = count < MAX_SECTORS ? count : MAX_SECTORS;
        if ((uint64_t)sector + n > b->sectors) return -1;
        for (uint32_t done = 0; done < n * 512; done += PAGE_SIZE) {
            uint32_t len = n * 512 - done < PAGE_SIZE ? n * 512 - done : PAGE_SIZE;
            memcpy(P2V(b->bounce[done / PAGE_SIZE]), in + done, len);
        }
        if (!transfer(b, sector, n, true)) return -1;
        in += n * 512;
        sector += n;
        count -= n;
    }
    return 0;
}

/* The IDE controller's bus-master base (BAR4), or 0. */
static uint16_t find_bus_master(void) {
    for (uint32_t bus = 0; bus < 8; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            for (uint32_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (fn == 0) break;
                    continue;
                }
                uint32_t cls = pci_read(bus, dev, fn, 8) >> 8;   /* class, subclass, prog-if */
                /* IDE, bus master, primary channel in compatibility mode. */
                if ((cls >> 8) != 0x0101 || !(cls & 0x80) || (cls & 0x01)) continue;
                uint32_t bar = pci_read(bus, dev, fn, 0x20);
                if (!(bar & 1) || !(bar & 0xFFFC)) continue;
                /* I/O space and bus mastering on. */
                uint32_t cmd = pci_read(bus, dev, fn, 4);
                pci_write(bus, dev, fn, 4, (cmd & 0xFFFF0000u) | (cmd & 0xFFFF) | 0x5);
                kprintf("boot: IDE bus master %02x:%02x.%u (%04x:%04x) at port %x\n", bus, dev, fn,
                        id & 0xFFFF, id >> 16, bar & 0xFFFC);
                return (uint16_t)(bar & 0xFFFC);
            }
        }
    }
    return 0;
}

fat_block_device_t* bmide_boot_bind(fat_block_device_t* pio) {
    if (!pio || !pio->private_data) return 0;
    bmide_t* b = &g_bm;
    b->bm = find_bus_master();
    if (!b->bm) return 0;
    b->drive = ((checkpoint_ide_binding_t*)pio->private_data)->drive;
    b->sectors = pio->total_sectors;
    b->prdt = pmm_alloc();
    for (int i = 0; i < BOUNCE_FRAMES; i++) b->bounce[i] = pmm_alloc();
    for (int i = 0; i < BOUNCE_FRAMES; i++) {
        if (!b->bounce[i] || b->bounce[i] >> 32) return 0;            /* 32-bit PRD addresses */
    }
    if (!b->prdt || b->prdt >> 32) return 0;
    /* The same sectors by both paths, before the store trusts DMA. */
    static uint8_t want[4096], got[4096];
    uint32_t n = b->sectors < 8 ? b->sectors : 8;
    if (!n || pio->read_sectors(pio->private_data, 0, n, want) != 0 || dev_read(b, 0, n, got) != 0 ||
        memcmp(want, got, n * 512) != 0) {
        kprintf("boot: IDE bus-master DMA does not read what PIO reads; using PIO\n");
        return 0;
    }
    g_dev.read_sectors = dev_read;
    g_dev.write_sectors = dev_write;
    g_dev.sector_size = 512;
    g_dev.total_sectors = b->sectors;
    g_dev.private_data = b;
    return &g_dev;
}
