; The user programs the kernel can run (#220), embedded as ELF images.
; The Makefile builds build/user/*.elf before assembling this file.

%macro USER_IMAGE 1
global user_image_%1
global user_image_%1_end
align 16
user_image_%1:
    incbin %str(build/user/%1.elf)
user_image_%1_end:
%endmacro

section .rodata
USER_IMAGE heisenbug
USER_IMAGE noise
USER_IMAGE counter
USER_IMAGE hello

section .note.GNU-stack noalloc noexec nowrite progbits
