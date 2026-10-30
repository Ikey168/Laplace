/* Laplace: the kernel-state format of a keyframe (#243).
 *
 * A keyframe's kernel state is one blob of sections, one per kernel subsystem
 * (processes, CPUID answers, the clock, open files, ...). Each subsystem
 * registers a save, a restore, and optionally a checksum callback; the format
 * has no fixed caps on processes, regions, or any other object, and each
 * section carries its own version and CRC.
 *
 *   header      magic, format version, section count, total size, CRC of the
 *               section table
 *   table       per section: id, version, offset, size, CRC of its bytes
 *   sections    the bytes each subsystem's save wrote
 *
 * Restore runs the registered subsystems in registration order, each on its
 * own section, so a subsystem whose objects refer to another's (an fd to a
 * process) registers after it. A blob with a section no registered subsystem
 * claims, or without a section a registered subsystem needs, is refused: a
 * keyframe must rebuild the whole machine or nothing.
 *
 * Saves must be deterministic (same state, same bytes); they run twice, once
 * to measure and once to write. Pure and host-tested (tests/test_kstate_format.c).
 */

#ifndef KSTATE_FORMAT_H
#define KSTATE_FORMAT_H

#include <stdint.h>
#include <stdbool.h>

#define KSF_MAGIC        0x31464B53504C4C00ULL   /* "\0LLPSKF1" */
#define KSF_VERSION      1
#define KSF_MAX_SUBSYS   16                       /* subsystems, not objects */

#define KSF_OK            0
#define KSF_ERR_PARAM    -1
#define KSF_ERR_FULL     -2   /* registry full, or output buffer too small */
#define KSF_ERR_CORRUPT  -3   /* bad magic, version, bounds, or CRC */
#define KSF_ERR_UNKNOWN  -4   /* a section no registered subsystem claims */
#define KSF_ERR_MISSING  -5   /* a registered subsystem's section is absent */
#define KSF_ERR_RESTORE  -6   /* a subsystem's restore failed */

/* Writer: appends while within cap and always counts, so a pass with cap 0
 * measures. */
typedef struct {
    uint8_t* buf;
    uint32_t cap;
    uint32_t len;
} ksf_writer_t;

void ksf_put(ksf_writer_t* w, const void* data, uint32_t len);
void ksf_put_u32(ksf_writer_t* w, uint32_t v);
void ksf_put_u64(ksf_writer_t* w, uint64_t v);

/* Reader over one section. Every get past the end fails and sets ok = false. */
typedef struct {
    const uint8_t* buf;
    uint32_t       len;
    uint32_t       pos;
    bool           ok;
} ksf_reader_t;

bool     ksf_get(ksf_reader_t* r, void* out, uint32_t len);
uint32_t ksf_get_u32(ksf_reader_t* r);
uint64_t ksf_get_u64(ksf_reader_t* r);
static inline uint32_t ksf_left(const ksf_reader_t* r) { return r->len - r->pos; }

typedef struct {
    uint32_t    id;         /* stable across builds; unique per subsystem */
    const char* name;       /* for reports: "procs", "files", ... */
    uint32_t    version;    /* of this subsystem's section layout */
    void     (*save)(ksf_writer_t* w, void* ctx);
    /* Rebuild from the section; `version` is the one it was written with.
     * Returns 0 on success. Must consume the whole section. */
    int      (*restore)(ksf_reader_t* r, uint32_t version, void* ctx);
    /* Checksum of the subsystem's live state, for divergence detection; NULL
     * when another component already covers it. */
    uint32_t (*checksum)(void* ctx);
    void*       ctx;
} ksf_subsys_t;

typedef struct {
    ksf_subsys_t subs[KSF_MAX_SUBSYS];
    uint32_t     count;
} ksf_registry_t;

void ksf_registry_init(ksf_registry_t* reg);
/* Register a subsystem; KSF_ERR_PARAM for a duplicate id or missing callbacks. */
int  ksf_register(ksf_registry_t* reg, const ksf_subsys_t* sub);
const ksf_subsys_t* ksf_find(const ksf_registry_t* reg, uint32_t id);

/* The blob's size for the current state. */
uint32_t ksf_measure(const ksf_registry_t* reg);
/* Write the blob into out[0..cap). Returns its size, or KSF_ERR_FULL. */
int64_t  ksf_encode(const ksf_registry_t* reg, uint8_t* out, uint32_t cap);
/* Check a blob's structure and CRCs without restoring anything. */
int      ksf_validate(const uint8_t* blob, uint32_t len);
/* Validate, then restore every registered subsystem from its section. On
 * KSF_ERR_RESTORE, *failed_id names the subsystem (may be NULL). */
int      ksf_restore(const ksf_registry_t* reg, const uint8_t* blob, uint32_t len,
                     uint32_t* failed_id);
/* Locate one section (for tools and tests). */
bool     ksf_section(const uint8_t* blob, uint32_t len, uint32_t id, ksf_reader_t* out,
                     uint32_t* version);

#endif /* KSTATE_FORMAT_H */
