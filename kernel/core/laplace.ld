/* Laplace kernel linker script (#219, #238).
 *
 * The kernel runs in the top 2 GiB (KERNEL_VMA, -mcmodel=kernel) and is loaded
 * at 1 MiB physical: every section's load address is its link address minus
 * KERNEL_VMA. The lower half of every address space belongs to user programs
 * (Linux binaries link at 0x400000).
 *
 * One loadable segment keeps file offsets in lockstep with load addresses,
 * which the Multiboot a.out-kludge loader relies on (it copies the file image
 * from the header's offset up to _load_end verbatim, then zeroes to _bss_end).
 */
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    image PT_LOAD FLAGS(7);
}

SECTIONS
{
    . = KERNEL_VMA + 0x100000;
    _kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VMA) {
        KEEP(*(.multiboot))
        *(.text .text.*)
    } :image

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    } :image

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    } :image

    _load_end = .;

    .bss ALIGN(4096) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss .bss.*)
    } :image

    . = ALIGN(4096);
    _bss_end = .;
    _kernel_end = .;

    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
        *(.eh_frame .eh_frame_hdr)
    }
}
