/* File system probe (#247): the calls programs make on files, against the
 * machine's filesystem. Reads /etc/motd from the seed image (if there is
 * one), then creates, writes, reads, seeks, maps, renames, links, lists,
 * truncates, and removes files and directories under /tmp, between keyframes.
 * Prints what it found and a checksum kept in memory, so a replay that
 * restored the filesystem differently ends in a different state. */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

static uint32_t g_sum = 2166136261u;
static void mix(const void* p, size_t n) {
    const uint8_t* b = p;
    for (size_t i = 0; i < n; i++) g_sum = (g_sum ^ b[i]) * 16777619u;
}

static volatile uint32_t g_spin;
static void spin(void) {
    for (uint32_t i = 0; i < 300000; i++) g_spin += i;
    getpid();   /* a kernel entry: a keyframe may fall here */
}

#define CHECK(c, what) do { if (!(c)) { printf("fileprobe: FAILED: %s (errno %d)\n", what, errno); return 1; } } while (0)

int main(void) {
    char buf[8192];

    /* The seed image. */
    int fd = open("/etc/motd", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        CHECK(n > 0, "read /etc/motd");
        buf[n] = 0;
        mix(buf, (size_t)n);
        printf("fileprobe: /etc/motd: %s", buf);
        close(fd);
    } else {
        printf("fileprobe: no seed image\n");
    }

    /* Create, write in pieces, read back. */
    CHECK(mkdir("/tmp/work", 0755) == 0, "mkdir");
    CHECK(chdir("/tmp/work") == 0, "chdir");
    CHECK(getcwd(buf, sizeof(buf)) && strcmp(buf, "/tmp/work") == 0, "getcwd");
    FILE* f = fopen("data.txt", "w");
    CHECK(f, "fopen w");
    for (int i = 0; i < 2000; i++) fprintf(f, "%05d:%08x\n", i, (unsigned)(i * 2654435761u));
    CHECK(fclose(f) == 0, "fclose");
    spin();
    struct stat st;
    CHECK(stat("data.txt", &st) == 0 && st.st_size == 2000 * 15 && S_ISREG(st.st_mode), "stat size");
    f = fopen("/tmp/work/data.txt", "r");
    CHECK(f, "fopen r");
    unsigned long total = 0;
    int lines = 0, idx;
    unsigned v;
    while (fscanf(f, "%d:%x\n", &idx, &v) == 2) { total += v; lines++; }
    fclose(f);
    CHECK(lines == 2000, "read back every line");
    mix(&total, sizeof(total));
    printf("fileprobe: wrote and read back %d lines (%ld bytes), total %lu\n", lines, (long)st.st_size, total);

    /* Seek, pread/pwrite past the end (a hole), writev, append. */
    fd = open("data.txt", O_RDWR);
    CHECK(fd >= 0, "open rdwr");
    CHECK(lseek(fd, 15 * 1000, SEEK_SET) == 15 * 1000 && read(fd, buf, 15) == 15 && memcmp(buf, "01000:", 6) == 0, "lseek+read");
    CHECK(pwrite(fd, "HOLE", 4, 100000) == 4, "pwrite past the end");
    CHECK(pread(fd, buf, 8, 99996) == 8 && memcmp(buf, "\0\0\0\0HOLE", 8) == 0, "the hole reads as zeros");
    struct iovec iov[2] = { { "vec", 3 }, { "tor", 3 } };
    CHECK(lseek(fd, 0, SEEK_END) == 100004 && writev(fd, iov, 2) == 6, "writev at the end");
    int fd2 = dup(fd);
    CHECK(fd2 > fd && lseek(fd2, 0, SEEK_CUR) == 100010, "dup shares the offset");
    CHECK(fcntl(fd2, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd2, F_GETFD) == FD_CLOEXEC, "fcntl FD_CLOEXEC");
    close(fd2);
    CHECK(ftruncate(fd, 50000) == 0 && fstat(fd, &st) == 0 && st.st_size == 50000, "ftruncate");
    close(fd);
    spin();

    /* A private file mapping sees the file's bytes. */
    fd = open("data.txt", O_RDONLY);
    char* m = mmap(0, 50000, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(m != MAP_FAILED && memcmp(m + 15 * 7, "00007:", 6) == 0, "mmap a file");
    mix(m, 50000);
    munmap(m, 50000);
    close(fd);

    /* Names: rename, link, symlink, readlink, list, unlink, rmdir. */
    CHECK(rename("data.txt", "renamed.txt") == 0 && access("data.txt", F_OK) != 0, "rename");
    CHECK(link("renamed.txt", "hard.txt") == 0 && stat("hard.txt", &st) == 0 && st.st_nlink == 2, "link");
    CHECK(symlink("/tmp/work/renamed.txt", "soft") == 0, "symlink");
    ssize_t rl = readlink("soft", buf, sizeof(buf));
    CHECK(rl == 21 && memcmp(buf, "/tmp/work/renamed.txt", 21) == 0, "readlink");
    CHECK(lstat("soft", &st) == 0 && S_ISLNK(st.st_mode), "lstat a link");
    CHECK(mkdir("sub", 0700) == 0, "mkdir sub");
    int fds[3];
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), "sub/f%d", i);
        fds[i] = open(buf, O_CREAT | O_EXCL | O_WRONLY, 0644);
        CHECK(fds[i] >= 0 && write(fds[i], buf, 6) == 6, "create in sub");
        close(fds[i]);
    }
    CHECK(open("sub/f1", O_CREAT | O_EXCL | O_WRONLY, 0644) < 0 && errno == EEXIST, "O_EXCL");
    DIR* d = opendir(".");
    CHECK(d, "opendir");
    int entries = 0;
    struct dirent* de;
    char names[512] = "";
    while ((de = readdir(d))) {
        entries++;
        strcat(names, " ");
        strcat(names, de->d_name);
    }
    closedir(d);
    mix(names, strlen(names));
    printf("fileprobe: /tmp/work has%s\n", names);
    CHECK(entries == 6, "readdir sees . .. and four names");
    CHECK(rmdir("sub") != 0 && errno == ENOTEMPTY, "rmdir of a full directory fails");
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), "sub/f%d", i);
        CHECK(unlink(buf) == 0, "unlink");
    }
    CHECK(rmdir("sub") == 0, "rmdir");
    spin();

    /* An unlinked file lives while open. */
    fd = open("renamed.txt", O_RDONLY);
    CHECK(unlink("renamed.txt") == 0 && unlink("hard.txt") == 0, "unlink both names");
    CHECK(read(fd, buf, 6) == 6 && memcmp(buf, "00000:", 6) == 0, "an unlinked open file still reads");
    close(fd);
    CHECK(stat("soft", &st) != 0 && errno == ENOENT, "the dangling link");
    unlink("soft");

    /* Devices. */
    fd = open("/dev/null", O_WRONLY);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1, "/dev/null");
    close(fd);
    fd = open("/dev/urandom", O_RDONLY);
    CHECK(fd >= 0 && read(fd, buf, 16) == 16, "/dev/urandom");
    mix(buf, 16);
    close(fd);

    /* Leave one file behind for the keyframes. */
    fd = open("/tmp/keep.txt", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    for (int i = 0; i < 100; i++) write(fd, "persistent state\n", 17);
    close(fd);
    printf("fileprobe: all checks passed; sum %08x\n", g_sum);
    return 0;
}
