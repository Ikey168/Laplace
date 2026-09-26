/* Laplace user programs (#220): linked at 1 GiB, text read-only and data
 * writable in separate page-aligned segments. */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x40000000;
    .text : {
        *(.text.start)
        *(.text .text.*)
    } :text
    .rodata : {
        *(.rodata .rodata.*)
    } :text

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    } :data
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
        *(.eh_frame .eh_frame_hdr)
    }
}
