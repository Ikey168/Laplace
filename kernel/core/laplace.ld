/* Laplace kernel linker script (#219).
 *
 * One loadable segment at 1 MiB, identity-mapped by boot.asm. Keeping every
 * section in a single PT_LOAD keeps file offsets in lockstep with addresses,
 * which the Multiboot a.out-kludge loader relies on (it copies the file image
 * from the header's offset up to _load_end verbatim, then zeroes to _bss_end).
 */
ENTRY(_start)

PHDRS
{
    image PT_LOAD FLAGS(7);
}

SECTIONS
{
    . = 0x100000;
    _kernel_start = .;

    .text : {
        KEEP(*(.multiboot))
        *(.text .text.*)
    } :image

    .rodata ALIGN(4096) : {
        *(.rodata .rodata.*)
    } :image

    .data ALIGN(4096) : {
        *(.data .data.*)
    } :image

    _load_end = .;

    .bss ALIGN(4096) (NOLOAD) : {
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
