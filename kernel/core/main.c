/* Laplace kernel entry and boot sequence (#219, #221, #226, #231).
 *
 * Kernel command line (Multiboot; QEMU -append):
 *   run=a,b,c      programs to start on a cold boot (default heisenbug,noise,noise)
 *   root=PATH      the filesystem's seed image: a cpio module (default: the
 *                  first module that is a cpio archive, #247)
 *   interval=N     keyframe cadence in timer ticks (default 100; 1 tick = 1 ms)
 *   keyframes=N    retained keyframes, the rewind horizon (default 16)
 *   dirty=N        also take a keyframe once an epoch dirties N pages (default
 *                  8192, 32 MiB; 0: never)
 *   pool=N         without an IDE disk: the RAM disk's page pool in MiB
 *                  (default 64; it takes memory only as keyframes fill it)
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
#include "core/kstate.h"
#include "core/files.h"
#include "core/kio.h"
#include "core/ksig.h"
#include "core/preempt.h"
#include "core/kfpages.h"
#include "core/monitor.h"
#include "core/clock.h"
#include "checkpoint.h"
#include "checkpoint_ide_boot.h"
#include "time_record.h"
#include "entropy_record.h"
#include "divergence.h"
#include "divergence_scan.h"
#include "revbreak.h"
#include "replay_driver.h"

int strncmp(const char* a, const char* b, size_t n);
int strcmp(const char* a, const char* b);
size_t strlen(const char* s);

#define MB_FLAG_CMDLINE (1u << 2)

typedef struct {
    char     run[512];
    char     env[256];
    char     root[64];
    uint32_t interval;
    uint32_t keyframes;
    uint32_t stop_after;
    uint32_t dirty;
    uint32_t pool_mib;
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

/* Copy a token's value, dropping the double quotes that let it hold spaces
 * (run="/bin/lua /t.lua"). */
static void copy_value(char* dst, uint32_t cap, const char* v, uint32_t len) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && n + 1 < cap; i++) {
        if (v[i] != '"') dst[n++] = v[i];
    }
    dst[n] = 0;
}

static void parse_cmdline(const char* cmd) {
    const char* dflt = "heisenbug,noise,noise";
    for (uint32_t i = 0; dflt[i]; i++) g_opts.run[i] = dflt[i];
    g_opts.interval = 100;
    g_opts.keyframes = 16;
    g_opts.dirty = 8192;
    g_opts.pool_mib = 64;
    if (!cmd) return;
    while (*cmd) {
        while (*cmd == ' ') cmd++;
        const char* tok = cmd;
        bool quoted = false;
        while (*cmd && (quoted || *cmd != ' ')) {
            if (*cmd == '"') quoted = !quoted;
            cmd++;
        }
        uint32_t len = (uint32_t)(cmd - tok);
        if (len > 4 && strncmp(tok, "run=", 4) == 0) {
            copy_value(g_opts.run, sizeof(g_opts.run), tok + 4, len - 4);
        } else if (len > 5 && strncmp(tok, "root=", 5) == 0) {
            copy_value(g_opts.root, sizeof(g_opts.root), tok + 5, len - 5);
        } else if (len > 4 && strncmp(tok, "env=", 4) == 0) {
            copy_value(g_opts.env, sizeof(g_opts.env), tok + 4, len - 4);
        } else if (len > 9 && strncmp(tok, "interval=", 9) == 0) {
            g_opts.interval = parse_u32(tok + 9, len - 9);
        } else if (len > 10 && strncmp(tok, "keyframes=", 10) == 0) {
            g_opts.keyframes = parse_u32(tok + 10, len - 10);
        } else if (len > 11 && strncmp(tok, "stop_after=", 11) == 0) {
            g_opts.stop_after = parse_u32(tok + 11, len - 11);
        } else if (len > 6 && strncmp(tok, "dirty=", 6) == 0) {
            g_opts.dirty = parse_u32(tok + 6, len - 6);
        } else if (len > 5 && strncmp(tok, "pool=", 5) == 0) {
            g_opts.pool_mib = parse_u32(tok + 5, len - 5);
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
    if (g_opts.pool_mib < 8) g_opts.pool_mib = 8;
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

/* Split `s` at spaces into words (in place), at most `max`. */
static uint32_t split_words(char* s, const char** out, uint32_t max) {
    uint32_t n = 0;
    while (*s && n < max) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        out[n++] = s;
        while (*s && *s != ' ') s++;
    }
    return n;
}

/* run= is a comma-separated list of programs, each with its arguments:
 * run="/bin/lua /scripts/t.lua,noise". env= is the environment for every
 * Linux program, space-separated. */
static void spawn_run_list(void) {
    static char envbuf[sizeof(g_opts.env)];
    const char* envp[32];
    for (uint32_t i = 0; i < sizeof(envbuf); i++) envbuf[i] = g_opts.env[i];
    uint32_t envc = split_words(envbuf, envp, 32);

    const char* p = g_opts.run;
    while (*p) {
        static char entry[256];
        uint32_t n = 0;
        while (*p && *p != ',' && n < sizeof(entry) - 1) entry[n++] = *p++;
        entry[n] = 0;
        if (*p == ',') p++;
        const char* argv[32];
        uint32_t argc = split_words(entry, argv, 32);
        if (argc == 0) continue;
        const lp_image_t* img = proc_find_image(argv[0]);
        if (!img) {
            kprintf("boot: no program named '%s'\n", argv[0]);
            continue;
        }
        process_t* proc = proc_spawn_args(img, argc, argv, envc, envp);
        if (!proc) panic("boot: cannot start %s", argv[0]);
        kprintf("boot: started %s as pid %d\n", argv[0], proc->pid);
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

static uint64_t g_scan_count;
static bool     g_scan_ok;
static void scan_count(void* ctx, uint64_t step) {
    (void)ctx;
    if (step != g_scan_count || machine_step() != step) g_scan_ok = false;
    g_scan_count++;
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
    const kfp_stats_t* ps = kfp_stats();
    kprintf("selftest: inputs: %lu records, %lu payload bytes journaled\n", kio_stats()->records,
            kio_stats()->bytes);
    kprintf("selftest: keyframe pages: %lu written, %lu kept their slot; the newest keyframe has "
            "%lu pages and wrote %lu; pool %u of %u slots in use\n", ps->pages_written,
            ps->pages_reused, ps->last_pages, ps->last_written, ps->slots_used, ps->slots_total);
    tt_verify_t v;
    uint64_t t0 = clock_now_unrecorded();
    int rc = tt_verify(&v);
    uint64_t verify_ms = (clock_now_unrecorded() - t0) / 1000000;
    const preempt_stats_t* pm = preempt_stats();
    kprintf("selftest: preemption points: %lu recorded in user code (%lu inside string "
            "instructions, %lu pages hashed); replays found %lu by state (%lu inside string "
            "instructions, %lu run forward in the kernel) in %lu checks\n", pm->recorded,
            pm->recorded_rep, pm->record_pages, pm->matched, pm->matched_rep, pm->forwarded,
            pm->candidates);
    kprintf("selftest: verify rc=%d: %u epochs replayed, %u diverged\n", rc, v.epochs_checked,
            v.epochs_diverged);
    kprintf("selftest: verify took %lu ms\n", verify_ms);
    if (v.diverged) {
        kprintf("selftest: first divergence epoch %lu component %u (%s) expected %x actual %x, "
                "between steps %lu..%lu\n", v.first_epoch, v.first_component,
                kstate_component_name(v.first_component), v.expected, v.actual, v.diag_step_lo,
                v.diag_step_hi);
        if (v.leak_found) {
            kprintf("selftest: leak: %s at rip %lx in pid %u (step %lu insn %lu)\n", v.leak_what,
                    v.leak_rip, v.leak_pid, v.leak_step, v.leak_insn);
        }
    }
    bool ok = rc == TT_OK && !v.diverged && v.epochs_checked > 0;

    /* By pid: every restore rebuilds the process table, so a process_t
     * pointer does not survive tt_verify. */
    uint32_t hb_pid = 0;
    for (uint32_t i = 0; i < proc_count(); i++) {
        if (strncmp(proc_at(i)->name, "heisenbug", 9) == 0) hb_pid = (uint32_t)proc_at(i)->pid;
    }
    /* The detector must notice a corrupted replay. Only the heisenbug's
     * recording is sure to show it: programs that do not interact end in the
     * same state under another schedule, and a process that exits takes its
     * corrupted registers with it. Elsewhere the results are reported. */
    bool strict = hb_pid != 0;
    tt_verify_t bad;
    tt_perturb_next_replay(true);
    rc = tt_verify(&bad);
    if (!replay_perturb_applied()) {
        kprintf("selftest: perturbed replay: the oldest epoch has no inputs to perturb (skipped)\n");
    } else {
        kprintf("selftest: perturbed replay: %s (rc %d, epoch %lu component %u)\n",
                bad.diverged ? "divergence detected" : "NOT detected", rc, bad.first_epoch,
                bad.first_component);
        ok = ok && (bad.diverged || !strict);
    }

    /* ... and a corrupted register in a keyframe's extended state (#237). */
    tt_perturb_next_restore(true);
    rc = tt_verify(&bad);
    kprintf("selftest: perturbed XMM15 in a keyframe: %s (epoch %lu component %u)\n",
            bad.diverged ? "divergence detected" : "NOT detected", bad.first_epoch,
            bad.first_component);
    ok = ok && (bad.diverged || !strict);

    /* ... and a kernel subsystem restored wrongly, named by the report (#243).
     * The probe's state lasts the whole epoch, so this holds for any program. */
    kstate_perturb_next_restore(KS_ID_PROBE);
    rc = tt_verify(&bad);
    const char* who = bad.diverged ? kstate_component_name(bad.first_component) : "-";
    kprintf("selftest: perturbed kernel-state section 'probe': %s (epoch %lu component %u: %s)\n",
            bad.diverged ? "divergence detected" : "NOT detected", bad.first_epoch,
            bad.first_component, who);
    ok = ok && bad.diverged && strcmp(who, "probe") == 0;
    /* ... and the filesystem (#247), which every machine has. */
    kstate_perturb_next_restore(KS_ID_FS);
    rc = tt_verify(&bad);
    who = bad.diverged ? kstate_component_name(bad.first_component) : "-";
    kprintf("selftest: perturbed kernel-state section 'fs': %s (epoch %lu component %u: %s)\n",
            bad.diverged ? "divergence detected" : "NOT detected", bad.first_epoch,
            bad.first_component, who);
    ok = ok && bad.diverged && strcmp(who, "fs") == 0;
    /* ... and a replay whose input call is not the recorded one (#242),
     * caught at that call's step. */
    kio_perturb_record_next(true);
    rc = tt_verify(&bad);
    who = bad.diverged ? kstate_component_name(bad.first_component) : "-";
    if (!kio_perturb_record_applied()) {
        kprintf("selftest: input record with other arguments: the oldest epoch has no input "
                "records (skipped)\n");
    } else {
        kprintf("selftest: input record with other arguments: %s (epoch %lu component %u: %s, "
                "step %lu)\n", bad.diverged ? "divergence detected" : "NOT detected",
                bad.first_epoch, bad.first_component, who, bad.diag_step_lo);
        ok = ok && bad.diverged && strcmp(who, "inputs") == 0;
    }
    tt_verify(&v);   /* back to the end of the recording, clean */
    kprintf("selftest: restored pages: %lu frames kept, %lu from the cache, %lu read from the pool\n",
            kfp_stats()->restore_kept, kfp_stats()->restore_cached, kfp_stats()->restore_read);

    /* A scan of the newest epoch visits every position through the stop
     * point, and nothing else. */
    g_scan_count = 0;
    g_scan_ok = true;
    rc = tt_scan_epoch(w.newest, w.end_step + 1, scan_count, 0);
    kprintf("selftest: scan of epoch %lu visited %lu positions (want %lu) rc=%d\n",
            w.newest, g_scan_count, w.end_step + 1, rc);
    ok = ok && rc == TT_OK && g_scan_ok && g_scan_count == w.end_step + 1;
    tt_verify(&v);

    /* Who last wrote the heisenbug's batch_limit? Found by value, to the
     * exact instruction (#228). */
    if (hb_pid) {
        uint32_t pid = hb_pid;
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
        uint32_t ids[DIVERGE_MAX_COMPONENTS], a_sums[DIVERGE_MAX_COMPONENTS];
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

    const uint32_t* mbi = (const uint32_t*)P2V(mb_info);
    parse_cmdline((mbi[0] & MB_FLAG_CMDLINE) ? (const char*)P2V(mbi[4]) : 0);

    cpu_init();
    cpu_cpuid_init();
    mm_init(mbi);
    proc_register_modules(mbi);
    pic_init();
    pit_init(MACHINE_TIMER_HZ);
    clock_init();
    kprintf("boot: %lu MiB free, timer %u Hz, TSC %lu MHz\n", pmm_free_frames() / 256,
            MACHINE_TIMER_HZ, clock_base()->tsc_hz / 1000000);
    kprintf("boot: user FPU state: %s (%u bytes)\n",
            !fpu_has_xsave() ? "x87+SSE (FXSAVE)" :
            (fpu_xcr0() & XCR0_AVX) ? "x87+SSE+AVX (XSAVE)" : "x87+SSE (XSAVE)", fpu_area_size());
    kprintf("boot: user rdtsc/rdtscp emulated; user cpuid %s\n",
            cpu_cpuid_faulting() ? "answered from the recorded table (CPUID faulting)"
                                 : "native (no CPUID faulting on this CPU)");

    ktime_init();
    kentropy_init();
    g_rdrand = cpu_has_rdrand();
    kentropy_set_source(entropy_source, 0);
    machine_init();
    kstate_init();
    kio_init();
    const lp_image_t* seed = proc_seed_module(g_opts.root);
    files_init(seed ? seed->start : 0, seed ? (uint64_t)(seed->end - seed->start) : 0,
               seed ? seed->name : "");
    ksig_init();
    preempt_init();
    kdiverge_init();
    kdiverge_register_kernel_sources();
    checkpoint_init();
    checkpoint_set_context_fn(proc_context_record);

    /* Durable store: the first IDE drive, else a volatile RAM disk. */
    fat_block_device_t* dev = checkpoint_ide_boot_bind();
    if (dev) {
        kprintf("boot: recording to the IDE disk (%u sectors)\n", dev->total_sectors);
    } else {
        dev = tt_ram_device(tt_required_sectors(g_opts.keyframes, g_opts.pool_mib));
        if (!dev) panic("no disk and no memory for a RAM disk");
        kprintf("boot: no IDE disk; recording to a RAM disk (lost at power-off)\n");
    }
    tt_config_t cfg = { g_opts.interval, g_opts.keyframes, g_opts.stop_after, g_opts.dirty };
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
