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
USER_IMAGE tscprobe
USER_IMAGE rdrandleak
USER_IMAGE fpuprobe
USER_IMAGE musl_hello
USER_IMAGE musl_sysprobe
USER_IMAGE musl_vmprobe
USER_IMAGE musl_bigmem
USER_IMAGE musl_fileprobe
USER_IMAGE musl_inputprobe
USER_IMAGE musl_sleeper
USER_IMAGE musl_forkexec
USER_IMAGE musl_sigprobe
USER_IMAGE musl_spinner
USER_IMAGE musl_threads
USER_IMAGE musl_benchcpu
USER_IMAGE musl_benchalloc

section .note.GNU-stack noalloc noexec nowrite progbits
