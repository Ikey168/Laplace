/* Throwaway probe: is there an exact retired-branch counter on this host?
 * Counts user-mode branches over a fixed loop several times; an exact counter
 * gives the same number every run. */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int open_counter(uint32_t type, uint64_t config) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = type;
    a.config = config;
    a.disabled = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}

static volatile uint64_t sink;
static void __attribute__((noinline)) work(void) {
    for (uint64_t i = 0; i < 1000000; i++) if (i % 3 == 0) sink += i;
}

static void probe(const char* name, uint32_t type, uint64_t config) {
    int fd = open_counter(type, config);
    if (fd < 0) { printf("%-28s unavailable (%m)\n", name); return; }
    printf("%-28s", name);
    for (int r = 0; r < 5; r++) {
        uint64_t v = 0;
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
        work();
        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        if (read(fd, &v, sizeof v) != sizeof v) v = 0;
        printf(" %llu", (unsigned long long)v);
    }
    printf("\n");
    close(fd);
}

int main(void) {
    probe("hw branch-instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS);
    probe("hw instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
    probe("raw Intel r5101c4 (rr)", PERF_TYPE_RAW, 0x5101c4);
    probe("raw AMD r5100d1 (rr)", PERF_TYPE_RAW, 0x5100d1);
    return 0;
}
