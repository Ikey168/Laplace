/* IKOS Orthogonal Persistence - Input Journal (#161, epic #159)
 *
 * See include/checkpoint_journal.h and docs/architecture/time-travel.md.
 *
 * Crash consistency and layout mirror the snapshot store (kernel/snapshot_store.c):
 * the new journal is written to the inactive slot and a single superblock write
 * commits it. This core carries its own sector buffers (in the writer/reader
 * handles) so it needs no allocator.
 */

#include "crc32_ieee.h"
#include "checkpoint_journal.h"
#include <stddef.h>

/* Freestanding helpers provided by the kernel. */
extern void* memset(void* ptr, int value, size_t size);
extern void* memcpy(void* dest, const void* src, size_t size);

/* ============================ CRC32 ============================ */

/* IEEE 802.3 reflected CRC32 (poly 0xEDB88320), from crc32_ieee.h, which
 * keeps this core dependency-free and host-testable on its own. Identical to
 * snapshot_crc32. */
uint32_t journal_crc32(uint32_t crc, const void* data, uint32_t len) {
    return crc32_ieee(crc, data, len);
}

/* ===================== Sector I/O wrappers ===================== */

static int dev_read(journal_store_t* store, uint32_t sector, void* buf) {
    fat_block_device_t* d = store->dev;
    if (!d || !d->read_sectors) return JOURNAL_ERR_IO;
    return d->read_sectors(d->private_data, sector, 1, buf) == 0
               ? JOURNAL_OK : JOURNAL_ERR_IO;
}

static int dev_write(journal_store_t* store, uint32_t sector, const void* buf) {
    fat_block_device_t* d = store->dev;
    if (!d || !d->write_sectors) return JOURNAL_ERR_IO;
    return d->write_sectors(d->private_data, sector, 1, buf) == 0
               ? JOURNAL_OK : JOURNAL_ERR_IO;
}

static uint32_t slot_base_sector(journal_store_t* store, uint32_t slot) {
    /* slot 0 immediately follows the superblock; slot 1 follows slot 0. */
    return store->base_sector + 1 + slot * store->slot_sectors;
}

/* Number of event sectors needed to hold event_count events. */
static uint32_t event_sectors_for(uint32_t event_count) {
    return (event_count + JOURNAL_EVENTS_PER_SECTOR - 1) / JOURNAL_EVENTS_PER_SECTOR;
}

/* ===================== Superblock helpers ===================== */

static uint32_t superblock_crc(const journal_superblock_t* sb) {
    return journal_crc32(0, sb, offsetof(journal_superblock_t, superblock_crc));
}

/* Read and validate the superblock. On JOURNAL_OK, *sb is filled and its CRC
 * checks out. Returns JOURNAL_ERR_NO_JOURNAL when unformatted, JOURNAL_ERR_CRC
 * on a bad checksum. */
static int read_superblock(journal_store_t* store, journal_superblock_t* sb) {
    uint8_t sec[JOURNAL_SECTOR_SIZE];
    int rc = dev_read(store, store->base_sector, sec);
    if (rc != JOURNAL_OK) return rc;
    memcpy(sb, sec, sizeof(*sb));
    if (sb->magic != JOURNAL_SB_MAGIC) return JOURNAL_ERR_NO_JOURNAL;
    if (sb->superblock_crc != superblock_crc(sb)) return JOURNAL_ERR_CRC;
    return JOURNAL_OK;
}

/* ============================ API ============================ */

int journal_store_init(journal_store_t* store, fat_block_device_t* dev,
                       uint32_t base_sector, uint32_t slot_sectors) {
    if (!store || !dev || slot_sectors < 2) return JOURNAL_ERR_PARAM;
    memset(store, 0, sizeof(*store));
    store->dev = dev;
    store->base_sector = base_sector;
    store->slot_sectors = slot_sectors;
    store->max_events = (slot_sectors - 1) * JOURNAL_EVENTS_PER_SECTOR;
    store->initialized = true;
    return JOURNAL_OK;
}

int journal_store_format(journal_store_t* store) {
    if (!store || !store->initialized) return JOURNAL_ERR_STATE;
    uint8_t sec[JOURNAL_SECTOR_SIZE];
    journal_superblock_t sb;
    memset(sec, 0, sizeof(sec));
    memset(&sb, 0, sizeof(sb));
    sb.magic = JOURNAL_SB_MAGIC;
    sb.version = JOURNAL_FORMAT_VERSION;
    sb.active_slot = JOURNAL_NO_SLOT;
    sb.epoch = 0;
    sb.event_count = 0;
    sb.slot_crc = 0;
    sb.superblock_crc = superblock_crc(&sb);
    memcpy(sec, &sb, sizeof(sb));
    return dev_write(store, store->base_sector, sec);
}

int journal_store_begin(journal_store_t* store, uint64_t epoch,
                        uint64_t base_lclock, journal_writer_t* writer) {
    if (!store || !store->initialized || !writer) return JOURNAL_ERR_PARAM;

    /* Target the slot that is NOT currently active, so the last good journal is
     * left untouched until the superblock flip commits this one. */
    journal_superblock_t sb;
    uint32_t target = 0;
    int rc = read_superblock(store, &sb);
    if (rc == JOURNAL_OK && sb.active_slot == 0) {
        target = 1;
    } else {
        target = 0;
    }

    memset(writer, 0, sizeof(*writer));
    writer->store = store;
    writer->slot = target;
    writer->slot_base = slot_base_sector(store, target);
    writer->epoch = epoch;
    writer->base_lclock = base_lclock;
    writer->event_count = 0;
    writer->crc = 0;
    writer->buf_events = 0;
    writer->event_sector = 0;
    writer->active = true;
    return JOURNAL_OK;
}

/* Flush the writer's pending sector buffer to disk and fold it into the running
 * CRC. The buffer is padded with zeros beyond buf_events, so the bytes on disk
 * (and thus the CRC) are deterministic. */
static int flush_sector(journal_writer_t* w) {
    uint32_t sector = w->slot_base + 1 + w->event_sector; /* +1 skips the header */
    int rc = dev_write(w->store, sector, w->sector_buf);
    if (rc != JOURNAL_OK) return rc;
    w->crc = journal_crc32(w->crc, w->sector_buf, JOURNAL_SECTOR_SIZE);
    w->event_sector++;
    w->buf_events = 0;
    memset(w->sector_buf, 0, JOURNAL_SECTOR_SIZE);
    return JOURNAL_OK;
}

int journal_writer_append(journal_writer_t* writer, uint32_t type,
                          uint64_t lclock, uint64_t value) {
    return journal_writer_append_len(writer, type, lclock, value, 0);
}

int journal_writer_append_len(journal_writer_t* writer, uint32_t type,
                              uint64_t lclock, uint64_t value, uint32_t len) {
    if (!writer || !writer->active) return JOURNAL_ERR_STATE;
    if (writer->event_count >= writer->store->max_events) return JOURNAL_ERR_FULL;

    journal_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.epoch = writer->epoch;
    ev.lclock = lclock;
    ev.type = type;
    ev.len = len;
    ev.value = value;

    uint32_t off = writer->buf_events * JOURNAL_EVENT_SIZE;
    memcpy(writer->sector_buf + off, &ev, sizeof(ev));
    writer->buf_events++;
    writer->event_count++;

    if (writer->buf_events == JOURNAL_EVENTS_PER_SECTOR) {
        int rc = flush_sector(writer);
        if (rc != JOURNAL_OK) { writer->active = false; return rc; }
    }
    return JOURNAL_OK;
}

int journal_store_commit(journal_writer_t* writer) {
    if (!writer || !writer->active) return JOURNAL_ERR_STATE;
    journal_store_t* store = writer->store;

    /* Flush a trailing partial sector, if any. */
    if (writer->buf_events > 0) {
        int rc = flush_sector(writer);
        if (rc != JOURNAL_OK) { writer->active = false; return rc; }
    }

    /* Write the slot header into sector 0 of the (inactive) slot. */
    uint8_t sec[JOURNAL_SECTOR_SIZE];
    journal_slot_header_t hdr;
    memset(sec, 0, sizeof(sec));
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = JOURNAL_SLOT_MAGIC;
    hdr.epoch = writer->epoch;
    hdr.event_count = writer->event_count;
    hdr.slot_crc = writer->crc;
    hdr.base_lclock = writer->base_lclock;
    memcpy(sec, &hdr, sizeof(hdr));
    int rc = dev_write(store, writer->slot_base, sec);
    if (rc != JOURNAL_OK) { writer->active = false; return rc; }

    /* Flip the superblock. This single write is the atomic commit point. */
    journal_superblock_t sb;
    memset(sec, 0, sizeof(sec));
    memset(&sb, 0, sizeof(sb));
    sb.magic = JOURNAL_SB_MAGIC;
    sb.version = JOURNAL_FORMAT_VERSION;
    sb.active_slot = writer->slot;
    sb.epoch = writer->epoch;
    sb.event_count = writer->event_count;
    sb.slot_crc = writer->crc;
    sb.superblock_crc = superblock_crc(&sb);
    memcpy(sec, &sb, sizeof(sb));
    rc = dev_write(store, store->base_sector, sec);
    writer->active = false;
    return rc;
}

int journal_store_load(journal_store_t* store, journal_reader_t* reader) {
    if (!store || !store->initialized || !reader) return JOURNAL_ERR_PARAM;

    journal_superblock_t sb;
    int rc = read_superblock(store, &sb);
    if (rc != JOURNAL_OK) return rc;
    if (sb.active_slot == JOURNAL_NO_SLOT) return JOURNAL_ERR_NO_JOURNAL;

    uint32_t slot_base = slot_base_sector(store, sb.active_slot);

    /* Validate the slot header. */
    uint8_t sec[JOURNAL_SECTOR_SIZE];
    journal_slot_header_t hdr;
    rc = dev_read(store, slot_base, sec);
    if (rc != JOURNAL_OK) return rc;
    memcpy(&hdr, sec, sizeof(hdr));
    if (hdr.magic != JOURNAL_SLOT_MAGIC) return JOURNAL_ERR_CRC;
    if (hdr.epoch != sb.epoch || hdr.event_count != sb.event_count)
        return JOURNAL_ERR_CRC;

    /* Recompute the CRC over the event sectors and match both stamps. */
    uint32_t nsec = event_sectors_for(hdr.event_count);
    uint32_t crc = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        rc = dev_read(store, slot_base + 1 + i, sec);
        if (rc != JOURNAL_OK) return rc;
        crc = journal_crc32(crc, sec, JOURNAL_SECTOR_SIZE);
    }
    if (crc != hdr.slot_crc || crc != sb.slot_crc) return JOURNAL_ERR_CRC;

    memset(reader, 0, sizeof(*reader));
    reader->store = store;
    reader->slot_base = slot_base;
    reader->epoch = hdr.epoch;
    reader->event_count = hdr.event_count;
    reader->next_index = 0;
    reader->buf_sector = JOURNAL_NO_SLOT; /* nothing buffered yet */
    reader->valid = true;
    return JOURNAL_OK;
}

int journal_reader_next(journal_reader_t* reader, journal_event_t* out) {
    if (!reader || !reader->valid || !out) return JOURNAL_ERR_PARAM;
    if (reader->next_index >= reader->event_count) return JOURNAL_ERR_NO_JOURNAL;

    uint32_t sidx = reader->next_index / JOURNAL_EVENTS_PER_SECTOR;
    uint32_t within = reader->next_index % JOURNAL_EVENTS_PER_SECTOR;

    if (reader->buf_sector != sidx) {
        int rc = dev_read(reader->store, reader->slot_base + 1 + sidx,
                          reader->sector_buf);
        if (rc != JOURNAL_OK) return rc;
        reader->buf_sector = sidx;
    }

    memcpy(out, reader->sector_buf + within * JOURNAL_EVENT_SIZE, sizeof(*out));
    reader->next_index++;
    return JOURNAL_OK;
}
