# Where Laplace records: TCG, KVM, and real machines (#236)

The kernel records and replays on one x86-64 CPU. What it needs from the
machine, and what each platform has been checked to provide:

| Platform | Status | PMU (boot log) | Disk for the recording |
|----------|--------|----------------|------------------------|
| QEMU TCG (`qemu-system-x86_64`, any `-cpu`) | every gate in CI | `boot: PMU: none on this AuthenticAMD CPU` (or GenuineIntel with `-cpu` Intel models): TCG emulates no performance counters | IDE (`-drive if=ide`), AHCI (`-device ahci`), else a RAM disk |
| QEMU/KVM on GitHub-hosted runners (AMD EPYC 7763, Azure) | `make selftest`, the preemption gate, and the e2e gate in CI (`-enable-kvm -cpu host`) | none: the host disables PMU virtualization (`kvm_amd: PMU virtualization is disabled`) | as above |
| QEMU/KVM on a host with a vPMU | expected to work; not in CI | the counters are found and probed; see below | as above |
| Bare metal | **untested**: no real machine has been booted for this issue | unknown | AHCI port 0, the first SATA disk (no NVMe yet) |

## What the boot log says

`kernel/core/pmu.c` looks for performance counters at boot:

- Intel: CPUID leaf 0xA (architectural performance monitoring) with the
  branch-instructions-retired event available.
- AMD and Hygon: the core performance counters (`PerfCtrExtCore`, else the
  four legacy ones).

It then programs one counter for retired conditional branches (Intel
`BR_INST_RETIRED.CONDITIONAL`, AMD event 0xD1, the events rr counts) and runs
a loop of 10000 branches twice. The counter is called exact only if both runs
agree and count the loop. The boot log then has one of two forms (the second
is the format; no CI machine prints it):

```
boot: PMU: none on this AuthenticAMD CPU (no architectural performance counters; QEMU TCG and KVM without a vPMU look like this)
boot: PMU: <vendor>, version V, N counters x W bits; retired conditional branches count exactly (C for 10000)
```

Preemption points (#245) need no counter: replay finds each one by its
instruction address and the machine state, so every platform above records
and replays the same way. A counter would only make a replay of a preempted
hot loop faster, by skipping most of the breakpoint hits. When none is exact,
the boot log says `preemption points are found by state alone`. The counter
path itself is not implemented yet, because no CI machine has a PMU to test
it on.

## KVM

Under KVM the selftest exercises TF single-stepping (reverse-stepi), the
debug registers (hardware breakpoints and watchpoints), and the trapped TSC
(CR4.TSD; `rdtsc` faults to the kernel and is journaled). With
`-cpu host`, user `cpuid` is answered from the recorded table through CPUID
faulting where the host exposes it.

To run the gates under KVM locally:

```bash
make selftest QEMU_FLAGS="-m 256M -display none -no-reboot -enable-kvm -cpu host"
QEMU_ACCEL=kvm bash tests/qemu/preempt_gate.sh
QEMU_ACCEL=kvm python3 tests/qemu/timetravel_e2e.py
```

## The AHCI disk

Most current machines have no legacy IDE controller. `kernel/core/ahci.c`
finds the first AHCI host bus adapter on PCI (class 01:06:01), maps its
registers uncached (`mm_mmio`), and brings up the first port with a SATA
disk. It polls: READ/WRITE DMA EXT through a 64 KiB bounce buffer, then FLUSH
CACHE EXT after every write, so a keyframe that was committed survives a power
cut. At boot the store is chosen in this order: the IDE disk, then the AHCI
disk, then the RAM disk.

## Which CPU the guest needs

The kernel and Laplace's own programs are built for the baseline x86-64 ISA
(`-march=x86-64`) and boot on any x86-64 CPU, including QEMU's default
`qemu64`. The Linux programs link against the build host's musl (and the
glibc gate runs the host's `lua5.4`), so they need what that distribution was
compiled for. Ubuntu 26.04 on GitHub's runners is built for x86-64-v3 (BMI2,
AVX2, FMA): its musl `libc.a` alone has a few hundred such instructions,
and under `qemu64` they raise #UD. So:

- under TCG, CI runs QEMU through `tools/qemu-tcg-max` (QEMU's `max` CPU,
  which emulates them on any host), and `tools/laplace-run` uses `-cpu max`
  unless `--cpu` names another model;
- under KVM the guest has the host's CPU (`-cpu host`);
- on bare metal, a build made on such a distribution needs a CPU with
  x86-64-v3 (Intel Haswell, AMD Excavator, or later). A build made on a
  baseline distribution runs anywhere.

## The IDE disk under KVM

The IDE driver (`kernel/ide_driver.c`) moves each sector through the data
port, and under KVM every `outs` element is an exit to QEMU: 256 per sector.
On a GitHub-hosted runner, where KVM itself runs nested, writing one keyframe
then took minutes. `kernel/core/bmide.c` moves the data by bus-master DMA
instead, when the IDE controller has a bus master (PIIX, class 01:01 with
prog-if bit 7, as in QEMU's default machine). It uses READ/WRITE DMA EXT
through a 64 KiB bounce buffer, polled, and is used once a test read
matches the PIO one. The boot log says which path it took:
`boot: recording to the IDE disk (131072 sectors, bus-master DMA)`.

```bash
LAPLACE_DISK=ahci bash scripts/test/qemu_persistence_demo.sh   # four power cuts on AHCI
```

## Bare metal: what is still missing

- The console and both debug channels (gdb, MCP) are COM ports. Many current
  machines have none, and there is no USB serial or network transport yet
  (#253).
- No NVMe driver.
- Booting `make iso` (GRUB, BIOS) needs legacy boot (CSM); there is no UEFI
  image.
- The boot log line above has not been read on any real CPU, so no bare-metal
  machine is listed as working.
