/* Laplace: the machine's filesystem (#247).
 *
 * The filesystem is inside the recording. It starts from a read-only seed
 * image (a cpio "newc" archive passed as a Multiboot module), which the
 * recording pins by CRC: a seed file's bytes are read straight from the image
 * and need no journal. Everything programs create or change lives in memory
 * as kernel state: metadata (inodes, directories) goes in the keyframe's
 * kernel-state section, file pages go in the keyframe page pool next to user
 * memory, and both are checksummed at every epoch boundary.
 *
 * A seed file is copied into pages at its first change. A file page's entry
 * is a frame address with MEMFS_PAGE_KNOWN set while its content equals the
 * reference keyframe's (as PAGE_KF_KNOWN for user pages, #244); every write
 * clears it. Before changing a page or freeing one, the filesystem calls
 * env.before_change, so a pending keyframe writeback reads the old content.
 *
 * Pure: frames come from the environment's callbacks. Host-tested
 * (tests/test_memfs.c).
 */

#ifndef MEMFS_H
#define MEMFS_H

#include <stdint.h>
#include <stdbool.h>
#include "kstate_format.h"

#define MEMFS_REG  1
#define MEMFS_DIR  2
#define MEMFS_LNK  3

#define MEMFS_ROOT      1
#define MEMFS_NAME_MAX  255
#define MEMFS_PATH_MAX  4096
#define MEMFS_PAGE_KNOWN 1ULL

/* errno values returned negated (Linux numbering). */
#define MEMFS_ENOENT   2
#define MEMFS_EIO      5
#define MEMFS_EBADF    9
#define MEMFS_ENOMEM  12
#define MEMFS_EEXIST  17
#define MEMFS_EXDEV   18
#define MEMFS_ENOTDIR 20
#define MEMFS_EISDIR  21
#define MEMFS_EINVAL  22
#define MEMFS_EFBIG   27
#define MEMFS_ENOSPC  28
#define MEMFS_ENAMETOOLONG 36
#define MEMFS_ENOTEMPTY 39
#define MEMFS_ELOOP   40

typedef struct {
    uint64_t (*page_alloc)(void);           /* a zeroed frame, or 0 */
    void     (*page_free)(uint64_t frame);
    void*    (*page_ptr)(uint64_t frame);
    void     (*before_change)(void);
    uint64_t (*now_ns)(void);               /* file times */
} memfs_env_t;

typedef struct {
    uint32_t ino;
    uint16_t len;
    char*    name;
} memfs_dirent_t;

typedef struct {
    bool      used;
    uint16_t  type;
    uint16_t  mode;          /* permission bits */
    uint32_t  nlink;
    uint32_t  opens;         /* open files; an unlinked inode lives while > 0 */
    uint32_t  parent;        /* directories: ".." */
    uint64_t  size;
    uint64_t  mtime_ns, ctime_ns;
    bool      seeded;        /* content is seed[seed_off, seed_off + size) */
    uint64_t  seed_off;
    uint64_t* pages;         /* frame | MEMFS_PAGE_KNOWN; 0 = hole (zeros) */
    uint32_t  npages;        /* capacity of pages[] */
    memfs_dirent_t* ents;    /* directories, in creation order */
    uint32_t  nents, cap;
} memfs_inode_t;

typedef struct {
    memfs_env_t    env;
    memfs_inode_t* inodes;   /* indexed by ino; 0 unused */
    uint32_t       ninodes;  /* capacity */
    uint32_t       used;
    const uint8_t* seed;
    uint64_t       seed_len;
    uint32_t       seed_crc;
} memfs_t;

typedef struct {
    uint32_t ino;
    uint16_t type, mode;
    uint32_t nlink;
    uint64_t size;
    uint64_t blocks;         /* 512-byte blocks allocated */
    uint64_t mtime_ns, ctime_ns;
} memfs_stat_t;

/* Build the filesystem: the root, then the seed's entries (seed may be NULL),
 * then /tmp if the seed has none. Returns 0 or -errno (a malformed seed). */
int  memfs_init(memfs_t* fs, const memfs_env_t* env, const uint8_t* seed, uint64_t len);
void memfs_destroy(memfs_t* fs);

/* Resolve `path` from directory `cwd`. follow: resolve a final symlink. */
int  memfs_lookup(memfs_t* fs, uint32_t cwd, const char* path, bool follow, uint32_t* ino);
/* Resolve all but the last component: the parent directory and the name. */
int  memfs_parent(memfs_t* fs, uint32_t cwd, const char* path, uint32_t* dir, const char** name,
                  uint32_t* namelen);

/* Create a file, directory, or symlink (target for symlinks) under cwd. */
int  memfs_create(memfs_t* fs, uint32_t cwd, const char* path, uint16_t type, uint16_t mode,
                  const char* target, uint32_t* ino);
int  memfs_unlink(memfs_t* fs, uint32_t cwd, const char* path, bool dir);
int  memfs_rename(memfs_t* fs, uint32_t cwd, const char* from, const char* to);
int  memfs_link(memfs_t* fs, uint32_t cwd, const char* from, const char* to);

int64_t memfs_read(memfs_t* fs, uint32_t ino, uint64_t off, void* buf, uint64_t len);
int64_t memfs_write(memfs_t* fs, uint32_t ino, uint64_t off, const void* buf, uint64_t len);
int  memfs_truncate(memfs_t* fs, uint32_t ino, uint64_t size);
int  memfs_readlink(memfs_t* fs, uint32_t ino, char* buf, uint32_t cap);
int  memfs_stat(memfs_t* fs, uint32_t ino, memfs_stat_t* st);
int  memfs_chmod(memfs_t* fs, uint32_t ino, uint16_t mode);
int  memfs_set_mtime(memfs_t* fs, uint32_t ino, uint64_t ns);
/* Directory entry `index` ("." is 0, ".." is 1): its inode and name. */
int  memfs_dirent(memfs_t* fs, uint32_t dir, uint32_t index, uint32_t* ino, const char** name,
                  uint32_t* namelen);
/* The absolute path of directory `ino`. Returns its length or -errno. */
int  memfs_path(memfs_t* fs, uint32_t ino, char* buf, uint32_t cap);
/* Open-file references: an unlinked inode is freed with its last one. */
void memfs_open_ref(memfs_t* fs, uint32_t ino, int delta);
/* A regular file's bytes as one contiguous block, if it is an untouched seed
 * file (loading programs from the image). */
const uint8_t* memfs_seed_bytes(memfs_t* fs, uint32_t ino, uint64_t* len);

/* ---- Keyframes (#243, #244) ---- */

void memfs_save(memfs_t* fs, ksf_writer_t* w);
/* Rebuild the metadata; pages come back through memfs_attach_page. The seed
 * must be the recording's (same CRC and length). */
int  memfs_restore(memfs_t* fs, ksf_reader_t* r, uint32_t version);
int  memfs_attach_page(memfs_t* fs, uint32_t ino, uint32_t index, uint64_t frame);
/* Every allocated page, in ascending (ino, index) order, by entry pointer. */
typedef void (*memfs_page_fn)(void* ctx, uint32_t ino, uint32_t index, uint64_t* entry);
void memfs_for_each_page(memfs_t* fs, memfs_page_fn fn, void* ctx);
/* Checksum of everything programs can observe; page_crc gives a page's CRC. */
typedef uint32_t (*memfs_crc_fn)(uint32_t ino, uint32_t index, uint64_t entry);
uint32_t memfs_checksum(memfs_t* fs, memfs_crc_fn page_crc);

#endif /* MEMFS_H */
