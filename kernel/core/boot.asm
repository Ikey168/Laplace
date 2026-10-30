; Laplace kernel entry (#219, #238).
;
; A Multiboot (v1) image with the a.out-kludge address fields, so both QEMU's
; -kernel loader and GRUB load the 64-bit ELF without needing an ELF32 wrapper.
; The loader enters _start in 32-bit protected mode with paging off, at the
; kernel's load address (link address - KERNEL_VMA): until paging is on, every
; address here is written as `symbol - KERNEL_VMA`.
;
; Page tables built here (#238): the first 4 GiB of physical memory mapped
; three ways with 2 MiB pages:
;   identity        PML4[0]    only for the jump into long mode; mm_init drops it
;   direct map      PML4[256]  0xFFFF800000000000 + phys; mm_init extends it
;   kernel window   PML4[511]  0xFFFFFFFF80000000 + phys (the first GiB), where
;                              the kernel is linked (-mcmodel=kernel)
; The lower half of every address space then belongs to user programs. The
; kernel half (PML4[256..511]) is shared by every address space.

MB_MAGIC     equ 0x1BADB002
MB_FLAGS     equ 0x00010003          ; page-align modules | memory map | address fields
MB_CHECKSUM  equ -(MB_MAGIC + MB_FLAGS)

KERNEL_VMA   equ 0xFFFFFFFF80000000

extern _kernel_start
extern _load_end
extern _bss_end
extern laplace_main

global _start
global boot_pml4
global boot_pdpt_direct
global boot_pd
global boot_stack_top

section .multiboot
align 4
mb_header:
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    dd mb_header - KERNEL_VMA       ; header_addr
    dd _kernel_start - KERNEL_VMA   ; load_addr
    dd _load_end - KERNEL_VMA       ; load_end_addr
    dd _bss_end - KERNEL_VMA        ; bss_end_addr
    dd _start - KERNEL_VMA          ; entry_addr

section .text
bits 32
_start:
    cli
    mov esp, boot_stack_top - KERNEL_VMA
    mov [mb_magic - KERNEL_VMA], eax
    mov [mb_info - KERNEL_VMA], ebx

    ; Long mode needs CPUID leaf 0x80000001 bit 29 (LM).
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; Zero the boot tables (the loader zeroes .bss, but be explicit).
    mov edi, boot_pml4 - KERNEL_VMA
    xor eax, eax
    mov ecx, (7 * 4096) / 4
    rep stosd

    ; PML4[0] and PML4[256] -> the direct PDPT; PML4[511] -> the kernel PDPT.
    mov eax, boot_pdpt_direct - KERNEL_VMA
    or eax, 0x3                     ; present | writable
    mov [boot_pml4 - KERNEL_VMA], eax
    mov [boot_pml4 - KERNEL_VMA + 256 * 8], eax
    mov eax, boot_pdpt_kernel - KERNEL_VMA
    or eax, 0x3
    mov [boot_pml4 - KERNEL_VMA + 511 * 8], eax

    ; Direct PDPT[0..3] -> four PDs covering 0..4 GiB; kernel PDPT[510] -> the
    ; first of them (0..1 GiB at 0xFFFFFFFF80000000).
    mov edi, boot_pdpt_direct - KERNEL_VMA
    mov eax, boot_pd - KERNEL_VMA
    or eax, 0x3
    mov ecx, 4
.fill_pdpt:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 4096
    add edi, 8
    loop .fill_pdpt
    mov eax, boot_pd - KERNEL_VMA
    or eax, 0x3
    mov [boot_pdpt_kernel - KERNEL_VMA + 510 * 8], eax

    ; The four PDs: 2048 x 2 MiB supervisor pages, physical 0..4 GiB.
    mov edi, boot_pd - KERNEL_VMA
    mov eax, 0x83                   ; present | writable | large (2 MiB)
    xor edx, edx                    ; high dword of the frame address
    mov ecx, 2048
.fill_pd:
    mov [edi], eax
    mov [edi + 4], edx
    add eax, 0x200000
    adc edx, 0
    add edi, 8
    loop .fill_pd

    mov eax, boot_pml4 - KERNEL_VMA
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5                  ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or eax, (1 << 8) | (1 << 0)     ; LME | SCE (SYSCALL/SYSRET, #238)
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)   ; PG | WP (kernel writes honour read-only PTEs)
    mov cr0, eax

    lgdt [gdt64_ptr - KERNEL_VMA]
    jmp 0x08:(long_mode_low - KERNEL_VMA)

.no_long_mode:
    ; Report on COM1 (no console exists yet) and halt.
    mov esi, no_lm_msg - KERNEL_VMA
.putc:
    lodsb
    test al, al
    jz .halt
    mov dx, 0x3F8
    out dx, al
    jmp .putc
.halt:
    hlt
    jmp .halt

bits 64
long_mode_low:
    ; Still running at the load address, through the identity map: jump to
    ; the link address.
    mov rax, long_mode_entry
    jmp rax

long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    mov rsp, boot_stack_top
    xor rbp, rbp
    mov edi, [rel mb_magic]
    mov esi, [rel mb_info]
    call laplace_main

.hang:
    cli
    hlt
    jmp .hang

section .rodata
no_lm_msg: db "Laplace: this CPU has no long mode (x86-64); halting.", 13, 10, 0

; Minimal GDT for the jump into long mode; cpu.c installs the full one (user
; segments + TSS) once C is running. The pointer holds the load address.
align 16
gdt64:
    dq 0
    dq 0x00209A0000000000           ; 0x08: kernel code, 64-bit
    dq 0x0000920000000000           ; 0x10: kernel data
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64 - KERNEL_VMA

section .data
mb_magic: dd 0
mb_info:  dd 0

section .bss
align 4096
boot_pml4:        resb 4096
boot_pdpt_direct: resb 4096
boot_pdpt_kernel: resb 4096
boot_pd:          resb 4 * 4096
boot_stack:
    resb 65536
boot_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
