/* Input probe (#242): read 10 MiB of journaled input (/dev/urandom, 64 KiB
 * per call) and a few clock reads, all within one epoch, and keep a hash of
 * every byte in memory, so a replay must hand back exactly the recorded
 * bytes. */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static uint8_t g_buf[65536];
static uint64_t g_hash = 1469598103934665603ULL;
static struct timespec g_ts[3];

int main(void) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) { printf("inputprobe: no /dev/urandom\n"); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &g_ts[0]);
    uint64_t total = 0;
    while (total < (10u << 20)) {
        ssize_t n = read(fd, g_buf, sizeof(g_buf));
        if (n <= 0) { printf("inputprobe: read failed\n"); return 1; }
        for (ssize_t i = 0; i < n; i++) g_hash = (g_hash ^ g_buf[i]) * 1099511628211ULL;
        total += (uint64_t)n;
    }
    clock_gettime(CLOCK_MONOTONIC, &g_ts[1]);
    clock_gettime(CLOCK_REALTIME, &g_ts[2]);
    close(fd);
    printf("inputprobe: read %llu bytes of journaled input; hash %016llx\n",
           (unsigned long long)total, (unsigned long long)g_hash);
    return 0;
}
