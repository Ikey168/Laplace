/* Laplace kernel entry and boot sequence (#219, #221, #226, #231).
 *
 * Kernel command line (Multiboot; QEMU -append):
 *   run=a,b,c      programs to start on a cold boot (default heisenbug,noise,noise)
 *   interval=N     keyframe cadence in timer ticks (default 100; 1 tick = 1 ms)
 *   keyframes=N    retained keyframes, the rewind horizon (default 16)
 *   fresh          ignore a recording on the disk and cold-boot
 *   stop_after=N   stop the machine for the debugger after N keyframes
 *   selftest       after the machine stops, verify the recording and halt
 *   exit           with selftest: leave QEMU through isa-debug-exit (port 0xF4)
 *                  with status 1 on success, 3 on failure
 */

#include "core/cpu.h"
#include "core/console.h"
#include "core/mm.h"
#include "core/proc.h"
#include "core/machine.h"
#include "core/timetravel.h"
#include "core/monitor.h"
#include "checkpoint.h"
#include "checkpoint_ide_boot.h"
#include "time_record.h"
#include "entropy_record.h"
#include "divergence.h"
#include "divergence_scan.h"
#include "revbreak.h"

int strncmp(const char* a, const char* b, size_t n);
size_t strlen(const char* s);

#define MB_FLAG_CMDLINE (1u << 2)

typedef struct {
    char     run[128];
    uint32_t interval;
    uint32_t keyframes;
    uint32_t stop_after;
    bool     fresh;
    bool     selftest;
    bool     exit;
} boot_options_t;

static boot_options_t g_opts;

static uint32_t parse_u32(const char* s, uint32_t len) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < len && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (uint32_t)(s[i] - '0');
    return v;
}

static void parse_cmdline(const char* cmd) {
    const char* dflt = "heisenbug,noise,noise";
    for (uint32_t i = 0; dflt[i]; i++) g_opts.run[i] = dflt[i];
    g_opts.interval = 100;
    g_opts.keyframes = 16;
    if (!cmd) return;
    while (*cmd) {
        while (*cmd == ' ') cmd++;
        const char* tok = cmd;
        while (*cmd && *cmd != ' ') cmd++;
        uint32_t len = (uint32_t)(cmd - tok);
        if (len > 4 && strncmp(tok, "run=", 4) == 0 && len - 4 < sizeof(g_opts.run)) {
            for (uint32_t i = 0; i < sizeof(g_opts.run); i++) g_opts.run[i] = 0;
            for (uint32_t i = 0; i < len - 4; i++) g_opts.run[i] = tok[4 + i];
        } else if (len > 9 && strncmp(tok, "interval=", 9) == 0) {
            g_opts.interval = parse_u32(tok + 9, len - 9);
        } else if (len > 10 && strncmp(tok, "keyframes=", 10) == 0) {
            g_opts.keyframes = parse_u32(tok + 10, len - 10);
        } else if (len > 11 && strncmp(tok, "stop_after=", 11) == 0) {
            g_opts.stop_after = parse_u32(tok + 11, len - 11);
        } else if (len == 5 && strncmp(tok, "fresh", 5) == 0) {
            g_opts.fresh = true;
        } else if (len == 8 && strncmp(tok, "selftest", 8) == 0) {
            g_opts.selftest = true;
        } else if (len == 4 && strncmp(tok, "exit", 4) == 0) {
            g_opts.exit = true;
        }
    }
    if (g_opts.interval == 0) g_opts.interval = 1;
    if (g_opts.keyframes < 2) g_opts.keyframes = 2;
    if (g_opts.keyframes > 64) g_opts.keyframes = 64;
}

/* Live entropy: RDRAND when the CPU has it, else a cycle-counter mix. Either
 * way it differs run to run; kentropy_fill journals whatever it returns. */
static bool g_rdrand;
static int entropy_source(void* ctx, void* buf, uint32_t len) {
    (void)ctx;
    static uint64_t state;
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t i = 0; i < len; i += 8) {
        uint64_t v;
        if (!g_rdrand || !cpu_rdrand64(&v)) {
            state += 0x9E3779B97F4A7C15ULL ^ rdtsc();
            v = state;
            v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
            v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL;
            v ^= v >> 31;
        }
        for (uint32_t b = 0; b < 8 && i + b < len; b++) out[i + b] = (uint8_t)(v >> (8 * b));
    }
    return 0;
}

static void spawn_run_list(void) {
    const char* p = g_opts.run;
    while (*p) {
        char name[32];
        uint32_t n = 0;
        while (*p && *p != ',' && n < sizeof(name) - 1) name[n++] = *p++;
        name[n] = 0;
        if (*p == ',') p++;
        const lp_image_t* img = proc_find_image(name);
        if (!img) {
            kprintf("boot: no program named '%s'\n", name);
            continue;
        }
        process_t* proc = proc_spawn(img);
        if (!proc) panic("boot: cannot start %s", name);
        kprintf("boot: started %s as pid %d\n", name, proc->pid);
    }
}

/* The heisenbug's batch_limit (user/laplace/heisenbug.c: ring[64] + 0). */
#define HEISENBUG_BATCH_LIMIT 0x40001100ULL
static uint32_t g_watch_pid;
static uint64_t watch_probe(void* ctx) {
    (void)ctx;
    process_t* p = proc_by_pid(g_watch_pid);
    uint32_t v = 0;
    if (p) uaccess_read(p->address_space, HEISENBUG_BATCH_LIMIT, &v, sizeof(v));
    return v;
}

/* QEMU's isa-debug-exit device: the process exits with status (v << 1) | 1. */
void boot_qemu_exit(uint8_t v) {
    outb(0xF4, v);
}

static bool selftest(void) {
    tt_window_t w;
    if (!tt_window(&w)) {
        kprintf("selftest: no recording\n");
        return false;
    }
    const tt_stats_t* s = tt_stats();
    kprintf("selftest: window epochs %lu..%lu (%u keyframes), end step %lu; %lu keyframes taken, "
            "%lu COW captures, %lu journals\n",
            w.oldest, w.newest, w.count, w.end_step, s->keyframes_taken, s->cow_captures,
            s->journals_written);
    tt_verify_t v;
    int rc = tt_verify(&v);
    kprintf("selftest: verify rc=%d: %u epochs replayed, %u diverged\n", rc, v.epochs_checked,
            v.epochs_diverged);
    if (v.diverged) {
        kprintf("selftest: first divergence epoch %lu component %u expected %x actual %x\n",
                v.first_epoch, v.first_component, v.expected, v.actual);
    }
    bool ok = rc == TT_OK && !v.diverged && v.epochs_checked > 0;

    /* The detector must notice a corrupted replay. */
    tt_verify_t bad;
    tt_perturb_next_replay(true);
    rc = tt_verify(&bad);
    kprintf("selftest: perturbed replay: %s (epoch %lu component %u)\n",
            bad.diverged ? "divergence detected" : "NOT detected", bad.first_epoch,
            bad.first_component);
    ok = ok && bad.diverged;
    tt_verify(&v);   /* back to the end of the recording, clean */

    /* Who last wrote the heisenbug's batch_limit? Found by value, to the
     * exact instruction (#228). */
    process_t* hb = 0;
    for (uint32_t i = 0; i < proc_count(); i++) {
        if (strncmp(proc_at(i)->name, "heisenbug", 9) == 0) hb = proc_at(i);
    }
    if (hb) {
        uint32_t pid = (uint32_t)hb->pid;
        g_watch_pid = pid;
        tt_pos_t end, w1, w2;
        tt_position(&end);
        uint32_t now = (uint32_t)watch_probe(0);
        rc = tt_last_change(pid, HEISENBUG_BATCH_LIMIT, 4, &w1);
        uint64_t rip1 = proc_by_pid(pid) ? proc_by_pid(pid)->context.rip : 0;
        uint32_t then = (uint32_t)watch_probe(0);
        kprintf("selftest: batch_limit %u at the stop; last written at (%lu,%lu,%lu), rip %lx, "
                "value there %u, rc=%d\n", now, w1.epoch, w1.step, w1.insn, rip1, then, rc);
        ok = ok && rc == TT_OK && then == now;
        tt_reverse_stepi();
        kprintf("selftest: one instruction earlier batch_limit was %u (rip %lx)\n",
                (uint32_t)watch_probe(0), proc_by_pid(pid)->context.rip);

        /* The same answer from a hardware write watchpoint + reverse-continue. */
        tt_goto_pos(&end);
        machine_hwbp_t wp = { true, pid, HEISENBUG_BATCH_LIMIT, 1, 4 };
        machine_set_hwbp(0, &wp);
        tt_hit_t hit;
        rc = tt_reverse_continue(&hit);
        tt_position(&w2);
        kprintf("selftest: watchpoint reverse-continue rc=%d hit=%d at (%lu,%lu,%lu) rip %lx\n",
                rc, hit.hit, w2.epoch, w2.step, w2.insn, proc_by_pid(pid)->context.rip);
        ok = ok && rc == TT_OK && hit.hit && w2.epoch == w1.epoch && w2.step == w1.step &&
             w2.insn == w1.insn;
        machine_clear_hwbps();

        /* Step back and forth over instructions and land on the same state. */
        tt_goto_pos(&end);
        uint32_t ids[KDIVERGE_COMPONENT_COUNT], a_sums[KDIVERGE_COMPONENT_COUNT];
        kdiverge_record_epoch(machine_epoch());
        uint32_t na = kdiverge_journal_sums(0, 0);
        const uint32_t* pids;
        const uint32_t* psums;
        kdiverge_journal_sums(&pids, &psums);
        for (uint32_t i = 0; i < na; i++) { ids[i] = pids[i]; a_sums[i] = psums[i]; }
        int back = 0, fwd = 0;
        for (int i = 0; i < 5; i++) back += tt_reverse_stepi() == TT_OK;
        tt_pos_t mid;
        tt_position(&mid);
        for (int i = 0; i < 5; i++) fwd += tt_stepi() == TT_OK;
        tt_pos_t again;
        tt_position(&again);
        kdiverge_record_epoch(machine_epoch());
        kdiverge_journal_sums(&pids, &psums);
        bool same = true;
        for (uint32_t i = 0; i < na; i++) same = same && ids[i] == pids[i] && a_sums[i] == psums[i];
        kprintf("selftest: 5 reverse-stepi to (%lu,%lu,%lu), 5 stepi back to (%lu,%lu,%lu): %s\n",
                mid.epoch, mid.step, mid.insn, again.epoch, again.step, again.insn,
                same ? "identical state" : "STATE DIFFERS");
        ok = ok && back == 5 && fwd == 5 && same && again.epoch == end.epoch &&
             again.step == end.step && again.insn == end.insn;

        /* An execution breakpoint on the store that clobbered batch_limit:
         * reverse-continue stops at its most recent execution, before it runs. */
        tt_goto_pos(&end);
        uint64_t store_pc = rip1 - 7;   /* mov %edi, 0x40001000(,%rax,4) is 7 bytes */
        machine_hwbp_t bp = { true, pid, store_pc, 0, 1 };
        machine_set_hwbp(0, &bp);
        rc = tt_reverse_continue(&hit);
        tt_pos_t b1;
        tt_position(&b1);
        uint64_t rip2 = proc_by_pid(pid)->context.rip;
        kprintf("selftest: breakpoint at %lx: reverse-continue rc=%d hit=%d at (%lu,%lu,%lu) rip %lx\n",
                store_pc, rc, hit.hit, b1.epoch, b1.step, b1.insn, rip2);
        ok = ok && rc == TT_OK && hit.hit && rip2 == store_pc;
        /* And forward again: continue stops at the next execution. */
        tt_pos_t b2;
        rc = tt_continue(&hit);
        tt_position(&b2);
        kprintf("selftest: continue rc=%d hit=%d at (%lu,%lu,%lu) rip %lx\n", rc, hit.hit,
                b2.epoch, b2.step, b2.insn, proc_by_pid(pid) ? proc_by_pid(pid)->context.rip : 0);
        ok = ok && (rc == TT_END || (rc == TT_OK && hit.hit));
        machine_clear_hwbps();
    }
    kprintf("selftest: %s\n", ok ? "PASSED" : "FAILED");
    return ok;
}

void laplace_main(uint32_t mb_magic, uint32_t mb_info) {
    serial_init(COM1);
    serial_init(COM2);
    serial_init(COM3);
    kprintf("\nLaplace: a time-traveling debugger built as an operating system\n");
    if (mb_magic != 0x2BADB002) panic("not booted by a Multiboot loader (magic %x)", mb_magic);

    const uint32_t* mbi = (const uint32_t*)(uint64_t)mb_info;
    parse_cmdline((mbi[0] & MB_FLAG_CMDLINE) ? (const char*)(uint64_t)mbi[4] : 0);

    cpu_init();
    mm_init(mb_info);
    pic_init();
    pit_init(MACHINE_TIMER_HZ);
    kprintf("boot: %lu MiB free, timer %u Hz\n", pmm_free_frames() / 256, MACHINE_TIMER_HZ);

    ktime_init();
    kentropy_init();
    g_rdrand = cpu_has_rdrand();
    kentropy_set_source(entropy_source, 0);
    machine_init();
    kdiverge_init();
    kdiverge_register_kernel_sources();
    checkpoint_init();

    /* Durable store: the first IDE drive, else a volatile RAM disk. */
    fat_block_device_t* dev = checkpoint_ide_boot_bind();
    if (dev) {
        kprintf("boot: recording to the IDE disk (%u sectors)\n", dev->total_sectors);
    } else {
        dev = tt_ram_device(tt_required_sectors(g_opts.keyframes));
        if (!dev) panic("no disk and no memory for a RAM disk");
        kprintf("boot: no IDE disk; recording to a RAM disk (lost at power-off)\n");
    }
    tt_config_t cfg = { g_opts.interval, g_opts.keyframes, g_opts.stop_after };
    bool had_keyframe = false;
    if (tt_init(dev, &cfg, &had_keyframe) != 0) panic("cannot arm the recording store");
    tt_bind_verbs();

    bool resumed = false;
    if (had_keyframe && !g_opts.fresh) {
        if (tt_resume_from_store() == TT_OK) {
            resumed = true;
            kprintf("boot: resumed the machine from keyframe %lu (%u processes)\n",
                    machine_epoch(), proc_count());
        } else {
            kprintf("boot: the recording on disk could not be restored; cold boot\n");
        }
    }
    if (!resumed) spawn_run_list();

    monitor_init();
    machine_set_mode(MACHINE_LIVE);
    if (resumed) machine_resume(); else machine_start();

    monitor_live_stopped();
    if (g_opts.selftest) {
        bool ok = selftest();
        if (g_opts.exit) boot_qemu_exit(ok ? 0 : 1);
        cpu_halt_forever();
    }
    monitor_run();
}
