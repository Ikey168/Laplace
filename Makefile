# Laplace: a time-traveling debugger built as an operating system.
#
#   make            build the kernel (build/laplace.elf) and its user programs
#   make run        boot it in QEMU (console on stdio, gdb on tcp:1235, MCP on tcp:1236)
#   make iso        build a GRUB rescue ISO (build/laplace.iso) for BIOS machines
#   make test       host unit tests + the in-QEMU end-to-end gates
#   make legacy     the pre-Laplace wildcard build (does not compile; see #233)
#
# Needs gcc, GNU ld, nasm, and (to boot) qemu-system-x86_64.

NASM    ?= nasm
CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy
QEMU    ?= qemu-system-x86_64

BUILD   := build
KBUILD  := $(BUILD)/kernel
UBUILD  := $(BUILD)/user

# Kernel: freestanding, no SSE/x87 state (the context switch saves only the
# general registers), -Werror so the bootable build stays green.
KCFLAGS := -m64 -std=gnu11 -O2 -g -ffreestanding -fno-pic -fno-pie \
           -fno-stack-protector -fno-omit-frame-pointer -mno-red-zone \
           -mgeneral-regs-only -fno-tree-loop-distribute-patterns \
           -Wall -Wextra -Werror -Iinclude
KLDFLAGS := -nostdlib -z max-page-size=0x1000 -z noexecstack --no-warn-rwx-segments -T kernel/core/laplace.ld

# The bootable core (#219, #220): boot, memory, processes, the machine engine,
# recording/replay integration, and the debug monitor.
CORE_ASM := kernel/core/boot.asm kernel/core/isr.asm
CORE_C   := kernel/core/main.c kernel/core/cpu.c \
            kernel/core/console.c kernel/core/klib.c kernel/core/mm.c \
            kernel/core/proc.c kernel/core/machine.c kernel/core/timetravel.c \
            kernel/core/monitor.c kernel/core/gdb_target.c

# The time-travel and persistence modules (host-tested; see tests/) and their
# kernel adapters.
TT_C := kernel/checkpoint.c kernel/snapshot_store.c kernel/checkpoint_extstate.c \
        kernel/checkpoint_barrier.c kernel/checkpoint_ide.c kernel/checkpoint_ide_boot.c \
        kernel/ide_driver.c kernel/checkpoint_journal.c kernel/journal_capture.c \
        kernel/journal_capture_sync.c kernel/journal_ring.c kernel/keyframe_ring.c \
        kernel/keyframe_store.c kernel/keyframe_store_sync.c kernel/sched_record.c \
        kernel/time_record.c kernel/time_record_sync.c kernel/entropy_record.c \
        kernel/entropy_record_sync.c kernel/replay_engine.c kernel/replay_engine_sync.c \
        kernel/replay_driver.c kernel/replay_driver_sync.c kernel/divergence.c \
        kernel/divergence_sync.c kernel/divergence_scan.c kernel/divergence_scan_sync.c \
        kernel/rewind.c kernel/rewind_sync.c kernel/reverse.c kernel/reverse_sync.c \
        kernel/revbreak.c kernel/revbreak_sync.c kernel/gdbstub.c kernel/gdbstub_sync.c \
        kernel/gdb_serial.c kernel/mcp.c kernel/mcp_sync.c kernel/mcp_server.c

KERNEL_C    := $(CORE_C) $(TT_C)
KERNEL_OBJS := $(patsubst %.asm,$(KBUILD)/%.o,$(CORE_ASM)) \
               $(patsubst %.c,$(KBUILD)/%.o,$(KERNEL_C)) \
               $(KBUILD)/kernel/core/user_images.o

# User programs (#220, #230, #231): ring-3 ELF images embedded in the kernel.
USER_PROGS := heisenbug noise counter hello
USER_ELFS  := $(patsubst %,$(UBUILD)/%.elf,$(USER_PROGS))
UCFLAGS := -m64 -std=gnu11 -O2 -g -ffreestanding -fno-pic -fno-pie \
           -fno-stack-protector -fno-omit-frame-pointer -mgeneral-regs-only \
           -fno-tree-loop-distribute-patterns -Wall -Wextra -Werror \
           -Iinclude -Iuser/laplace
ULDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack \
            -T user/laplace/user.ld

.PHONY: all kernel user run iso clean legacy

all: kernel

kernel: $(BUILD)/laplace.elf
user: $(USER_ELFS)

$(BUILD)/laplace.elf: $(KERNEL_OBJS) kernel/core/laplace.ld
	$(LD) $(KLDFLAGS) -o $@ $(KERNEL_OBJS)

$(KBUILD)/kernel/core/user_images.o: kernel/core/user_images.asm $(USER_ELFS)
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<

$(UBUILD)/%.elf: user/laplace/%.c user/laplace/ulib.h include/laplace/syscalls.h user/laplace/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $(UBUILD)/$*.o
	$(LD) $(ULDFLAGS) -o $@ $(UBUILD)/$*.o

$(KBUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -MMD -MP -c $< -o $@

$(KBUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g -F dwarf -o $@ $<

QEMU_FLAGS ?= -m 256M -display none -no-reboot
run: kernel
	$(QEMU) $(QEMU_FLAGS) -kernel $(BUILD)/laplace.elf -serial stdio

iso: kernel
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(BUILD)/laplace.elf $(BUILD)/iso/boot/laplace.elf
	printf 'set timeout=0\nset default=0\nmenuentry "Laplace" {\n  multiboot /boot/laplace.elf\n  boot\n}\n' > $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD)/laplace.iso $(BUILD)/iso

legacy:
	$(MAKE) -f Makefile.legacy all

clean:
	rm -rf $(BUILD)

-include $(shell find $(KBUILD) -name '*.d' 2>/dev/null)
