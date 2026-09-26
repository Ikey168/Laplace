; Laplace kernel entry (#219).
;
; A Multiboot (v1) image with the a.out-kludge address fields, so both QEMU's
; -kernel loader and GRUB load the 64-bit ELF without needing an ELF32 wrapper.
; The loader enters _start in 32-bit protected mode with paging off; this file
; identity-maps the first 1 GiB with 2 MiB pages, enables long mode, and calls
; laplace_main(multiboot_magic, multiboot_info) on the boot stack.
;
; The page tables built here stay live for the whole run: every process address
; space shares boot_pd (the supervisor-only kernel identity map) through its own
; PDPT, so physical frames below 1 GiB are directly addressable from the kernel.

MB_MAGIC     equ 0x1BADB002
MB_FLAGS     equ 0x00010003          ; page-align modules | memory map | address fields
MB_CHECKSUM  equ -(MB_MAGIC + MB_FLAGS)

extern _kernel_start
extern _load_end
extern _bss_end
extern laplace_main

global _start
global boot_pml4
global boot_pdpt
global boot_pd
global boot_stack_top

section .multiboot
align 4
mb_header:
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    dd mb_header            ; header_addr
    dd _kernel_start        ; load_addr
    dd _load_end            ; load_end_addr
    dd _bss_end             ; bss_end_addr
    dd _start               ; entry_addr

section .text
bits 32
_start:
    cli
    mov esp, boot_stack_top
    mov [mb_magic], eax
    mov [mb_info], ebx

    ; Long mode needs CPUID leaf 0x80000001 bit 29 (LM).
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; Zero the three boot tables (the loader zeroes .bss, but be explicit).
    mov edi, boot_pml4
    xor eax, eax
    mov ecx, (3 * 4096) / 4
    rep stosd

    ; PML4[0] -> PDPT, PDPT[0] -> PD. User space lives above 1 GiB, in PDPT
    ; entries each process owns; these two entries are kernel-only.
    mov eax, boot_pdpt
    or eax, 0x3                     ; present | writable
    mov [boot_pml4], eax
    mov eax, boot_pd
    or eax, 0x3
    mov [boot_pdpt], eax

    ; PD: 512 x 2 MiB supervisor pages identity-mapping 0..1 GiB.
    mov edi, boot_pd
    mov eax, 0x83                   ; present | writable | large (2 MiB)
    mov ecx, 512
.fill_pd:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill_pd

    mov eax, boot_pml4
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5                  ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or eax, 1 << 8                  ; LME
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)   ; PG | WP (kernel writes honour read-only PTEs)
    mov cr0, eax

    lgdt [gdt64_ptr]
    jmp 0x08:long_mode_entry

.no_long_mode:
    ; Report on COM1 (no console exists yet) and halt.
    mov esi, no_lm_msg
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
    mov edi, [mb_magic]
    mov esi, [mb_info]
    call laplace_main

.hang:
    cli
    hlt
    jmp .hang

section .rodata
no_lm_msg: db "Laplace: this CPU has no long mode (x86-64); halting.", 13, 10, 0

; Minimal GDT for the jump into long mode; cpu.c installs the full one (user
; segments + TSS) once C is running.
align 16
gdt64:
    dq 0
    dq 0x00209A0000000000           ; 0x08: kernel code, 64-bit
    dq 0x0000920000000000           ; 0x10: kernel data
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

section .data
mb_magic: dd 0
mb_info:  dd 0

section .bss
align 4096
boot_pml4: resb 4096
boot_pdpt: resb 4096
boot_pd:   resb 4096
boot_stack:
    resb 65536
boot_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
