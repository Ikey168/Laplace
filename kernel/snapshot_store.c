/* IKOS Orthogonal Persistence - On-disk Snapshot Store
 *
 * See include/snapshot_store.h and
 * docs/architecture/orthogonal-persistence.md. Issue #114 (epic #121).
 */

#include "crc32_ieee.h"
#include "snapshot_store.h"
#include <stddef.h>

/* Freestanding helpers provided by the kernel. */
extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);
extern void* memset(void* ptr, int value, size_t size);
extern void* memcpy(void* dest, const void* src, size_t size);

/* ============================ CRC32 ============================ */

/* IEEE 802.3 reflected CRC32 (poly 0xEDB88320), from crc32_ieee.h. Caller
 * seeds with 0 and feeds buffers in order. */
uint32_t snapshot_crc32(uint32_t crc, const void* data, uint32_t len) {
    return crc32_ieee(crc, data, len);
}

/* ===================== Sector I/O wrappers ===================== */

static int dev_read(snapshot_store_t* store, uint32_t sector, uint32_t count, void* buf) {
    fat_block_device_t* d = store->dev;
    if (!d || !d->read_sectors) return SNAPSHOT_ERR_IO;
    return d->read_sectors(d->private_data, sector, count, buf) == 0
               ? SNAPSHOT_OK : SNAPSHOT_ERR_IO;
}

static int dev_write(snapshot_store_t* store, uint32_t sector, uint32_t count, const void* buf) {
    fat_block_device_t* d = store->dev;
    if (!d || !d->write_sectors) return SNAPSHOT_ERR_IO;
    return d->write_sectors(d->private_data, sector, count, buf) == 0
               ? SNAPSHOT_OK : SNAPSHOT_ERR_IO;
}

static uint32_t slot_base_sector(snapshot_store_t* store, uint32_t slot) {
    /* slot 0 immediately follows the superblock; slot 1 follows slot 0. */
    return store->base_sector + 1 + slot * store->slot_sectors;
}

static uint32_t record_sector(uint32_t slot_base, uint32_t index) {
    /* slot header occupies sector 0 of the slot. */
    return slot_base + 1 + index * SNAPSHOT_SECTORS_PER_RECORD;
}

/* ===================== Superblock helpers ===================== */

static int read_superblock(snapshot_store_t* store, snapshot_superblock_t* out) {
    uint8_t sec[SNAPSHOT_SECTOR_SIZE];
    int rc = dev_read(store, store->base_sector, 1, sec);
    if (rc != SNAPSHOT_OK) return rc;
    memcpy(out, sec, sizeof(*out));
    return SNAPSHOT_OK;
}

static int write_superblock(snapshot_store_t* store, const snapshot_superblock_t* sb) {
    uint8_t sec[SNAPSHOT_SECTOR_SIZE];
    memset(sec, 0, sizeof(sec));
    memcpy(sec, sb, sizeof(*sb));
    return dev_write(store, store->base_sector, 1, sec);
}

/* CRC over the superblock up to (but not including) the superblock_crc field. */
static uint32_t superblock_crc(const snapshot_superblock_t* sb) {
    return snapshot_crc32(0, sb, offsetof(snapshot_superblock_t, superblock_crc));
}

static bool superblock_valid(const snapshot_superblock_t* sb) {
    if (sb->magic != SNAPSHOT_SB_MAGIC) return false;
    if (sb->version != SNAPSHOT_FORMAT_VERSION) return false;
    return sb->superblock_crc == superblock_crc(sb);
}

/* =========================== Public API =========================== */

int snapshot_store_init(snapshot_store_t* store, fat_block_device_t* dev,
                        uint32_t base_sector, uint32_t slot_sectors) {
    if (!store || !dev) return SNAPSHOT_ERR_PARAM;
    if (slot_sectors < 1 + SNAPSHOT_SECTORS_PER_RECORD) return SNAPSHOT_ERR_PARAM;
    if (dev->sector_size != SNAPSHOT_SECTOR_SIZE) return SNAPSHOT_ERR_PARAM;

    /* Region must fit: superblock + two slots. */
    uint32_t needed = 1 + 2 * slot_sectors;
    if ((uint64_t)base_sector + needed > dev->total_sectors) return SNAPSHOT_ERR_PARAM;

    memset(store, 0, sizeof(*store));
    store->dev = dev;
    store->base_sector = base_sector;
    store->slot_sectors = slot_sectors;
    store->max_records = (slot_sectors - 1) / SNAPSHOT_SECTORS_PER_RECORD;
    store->initialized = true;
    return SNAPSHOT_OK;
}

void snapshot_store_set_version(snapshot_store_t* store, uint32_t kernel_version) {
    if (store) {
        store->kernel_version = kernel_version;
    }
}

int snapshot_store_format(snapshot_store_t* store) {
    if (!store || !store->initialized) return SNAPSHOT_ERR_STATE;

    snapshot_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = SNAPSHOT_SB_MAGIC;
    sb.version = SNAPSHOT_FORMAT_VERSION;
    sb.active_slot = SNAPSHOT_NO_SLOT;
    sb.epoch = 0;
    sb.slot_crc = 0;
    sb.kernel_version = store->kernel_version;
    sb.superblock_crc = superblock_crc(&sb);
    return write_superblock(store, &sb);
}

int snapshot_store_begin(snapshot_store_t* store, uint64_t epoch,
                         snapshot_writer_t* writer) {
    if (!store || !store->initialized || !writer) return SNAPSHOT_ERR_PARAM;

    /* Choose the slot that is NOT currently active so the last good
     * checkpoint is never overwritten. */
    snapshot_superblock_t sb;
    uint32_t inactive;
    if (read_superblock(store, &sb) == SNAPSHOT_OK && superblock_valid(&sb) &&
        sb.active_slot <= 1) {
        inactive = sb.active_slot ^ 1u;
    } else {
        /* No valid checkpoint yet: write into slot 0. */
        inactive = 0;
    }

    memset(writer, 0, sizeof(*writer));
    writer->store = store;
    writer->slot = inactive;
    writer->slot_base = slot_base_sector(store, inactive);
    writer->epoch = epoch;
    writer->crc = 0;
    writer->active = true;
    return SNAPSHOT_OK;
}

int snapshot_writer_add_page(snapshot_writer_t* writer, uint32_t pid,
                             uint64_t virt_addr, uint32_t flags,
                             const void* page_data) {
    if (!writer || !writer->active || !page_data) return SNAPSHOT_ERR_PARAM;
    snapshot_store_t* store = writer->store;
    if (writer->record_count >= store->max_records) return SNAPSHOT_ERR_FULL;

    uint32_t base = record_sector(writer->slot_base, writer->record_count);

    /* Metadata sector (zero-padded to a full sector). */
    uint8_t meta[SNAPSHOT_SECTOR_SIZE];
    memset(meta, 0, sizeof(meta));
    snapshot_record_header_t* hdr = (snapshot_record_header_t*)meta;
    hdr->epoch = writer->epoch;
    hdr->pid = pid;
    hdr->flags = flags;
    hdr->virt_addr = virt_addr;
    hdr->page_bytes = SNAPSHOT_PAGE_SIZE;

    int rc = dev_write(store, base, 1, meta);
    if (rc != SNAPSHOT_OK) return rc;
    rc = dev_write(store, base + 1, SNAPSHOT_SECTORS_PER_PAGE, page_data);
    if (rc != SNAPSHOT_OK) return rc;

    /* CRC covers, per record in order: metadata sector then page data. */
    writer->crc = snapshot_crc32(writer->crc, meta, SNAPSHOT_SECTOR_SIZE);
    writer->crc = snapshot_crc32(writer->crc, page_data, SNAPSHOT_PAGE_SIZE);

    writer->record_count++;
    writer->page_count++;
    return SNAPSHOT_OK;
}

int snapshot_store_commit(snapshot_writer_t* writer) {
    if (!writer || !writer->active) return SNAPSHOT_ERR_STATE;
    snapshot_store_t* store = writer->store;

    /* 1. Slot header (sector 0 of the slot), carrying the record CRC. */
    uint8_t sec[SNAPSHOT_SECTOR_SIZE];
    memset(sec, 0, sizeof(sec));
    snapshot_slot_header_t* sh = (snapshot_slot_header_t*)sec;
    sh->magic = SNAPSHOT_SLOT_MAGIC;
    sh->epoch = writer->epoch;
    sh->record_count = writer->record_count;
    sh->page_count = writer->page_count;
    sh->slot_crc = writer->crc;
    int rc = dev_write(store, writer->slot_base, 1, sec);
    if (rc != SNAPSHOT_OK) return rc;

    /* 2. Flip the superblock. THIS single write is the commit point. */
    snapshot_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = SNAPSHOT_SB_MAGIC;
    sb.version = SNAPSHOT_FORMAT_VERSION;
    sb.active_slot = writer->slot;
    sb.epoch = writer->epoch;
    sb.slot_crc = writer->crc;
    sb.kernel_version = store->kernel_version;
    sb.superblock_crc = superblock_crc(&sb);
    rc = write_superblock(store, &sb);
    if (rc != SNAPSHOT_OK) return rc;

    writer->active = false;
    return SNAPSHOT_OK;
}

/* Recompute the CRC over a slot's record sectors and compare to its stored
 * slot_crc. Returns SNAPSHOT_OK if valid. */
static int validate_slot(snapshot_store_t* store, uint32_t slot_base,
                         const snapshot_slot_header_t* sh) {
    uint8_t meta[SNAPSHOT_SECTOR_SIZE];
    uint8_t* page = (uint8_t*)kmalloc(SNAPSHOT_PAGE_SIZE);
    if (!page) return SNAPSHOT_ERR_NOMEM;

    uint32_t crc = 0;
    int rc = SNAPSHOT_OK;
    for (uint32_t i = 0; i < sh->record_count; i++) {
        uint32_t base = record_sector(slot_base, i);
        rc = dev_read(store, base, 1, meta);
        if (rc != SNAPSHOT_OK) break;
        rc = dev_read(store, base + 1, SNAPSHOT_SECTORS_PER_PAGE, page);
        if (rc != SNAPSHOT_OK) break;
        crc = snapshot_crc32(crc, meta, SNAPSHOT_SECTOR_SIZE);
        crc = snapshot_crc32(crc, page, SNAPSHOT_PAGE_SIZE);
    }
    kfree(page);
    if (rc != SNAPSHOT_OK) return rc;
    return (crc == sh->slot_crc) ? SNAPSHOT_OK : SNAPSHOT_ERR_CRC;
}

int snapshot_store_load(snapshot_store_t* store, snapshot_reader_t* reader) {
    if (!store || !store->initialized || !reader) return SNAPSHOT_ERR_PARAM;
    memset(reader, 0, sizeof(*reader));

    snapshot_superblock_t sb;
    int rc = read_superblock(store, &sb);
    if (rc != SNAPSHOT_OK) return rc;
    if (!superblock_valid(&sb)) return SNAPSHOT_ERR_NO_CHECKPOINT;
    /* Reject a checkpoint written by a different kernel build (#139): its
     * persisted kernel state would not match this kernel's layout. */
    if (sb.kernel_version != store->kernel_version) return SNAPSHOT_ERR_VERSION;
    if (sb.active_slot > 1) return SNAPSHOT_ERR_NO_CHECKPOINT;

    uint32_t slot_base = slot_base_sector(store, sb.active_slot);

    /* Slot header. */
    uint8_t sec[SNAPSHOT_SECTOR_SIZE];
    rc = dev_read(store, slot_base, 1, sec);
    if (rc != SNAPSHOT_OK) return rc;
    snapshot_slot_header_t sh;
    memcpy(&sh, sec, sizeof(sh));
    if (sh.magic != SNAPSHOT_SLOT_MAGIC) return SNAPSHOT_ERR_NO_CHECKPOINT;
    if (sh.epoch != sb.epoch) return SNAPSHOT_ERR_NO_CHECKPOINT;
    if (sh.slot_crc != sb.slot_crc) return SNAPSHOT_ERR_CRC;
    if (sh.record_count > store->max_records) return SNAPSHOT_ERR_CRC;

    /* Full integrity check before yielding any data. */
    rc = validate_slot(store, slot_base, &sh);
    if (rc != SNAPSHOT_OK) return rc;

    reader->store = store;
    reader->slot_base = slot_base;
    reader->epoch = sh.epoch;
    reader->record_count = sh.record_count;
    reader->next_index = 0;
    reader->valid = true;
    return SNAPSHOT_OK;
}

int snapshot_reader_next(snapshot_reader_t* reader, snapshot_page_record_t* out) {
    if (!reader || !reader->valid || !out || !out->page_data) return SNAPSHOT_ERR_PARAM;
    if (reader->next_index >= reader->record_count) return SNAPSHOT_ERR_NO_CHECKPOINT;

    uint32_t base = record_sector(reader->slot_base, reader->next_index);
    uint8_t meta[SNAPSHOT_SECTOR_SIZE];
    int rc = dev_read(reader->store, base, 1, meta);
    if (rc != SNAPSHOT_OK) return rc;
    rc = dev_read(reader->store, base + 1, SNAPSHOT_SECTORS_PER_PAGE, out->page_data);
    if (rc != SNAPSHOT_OK) return rc;

    snapshot_record_header_t* hdr = (snapshot_record_header_t*)meta;
    out->epoch = hdr->epoch;
    out->pid = hdr->pid;
    out->flags = hdr->flags;
    out->virt_addr = hdr->virt_addr;

    reader->next_index++;
    return SNAPSHOT_OK;
}
