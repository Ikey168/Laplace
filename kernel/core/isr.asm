; Laplace trap entry, return, and the monitor <-> machine switch (#219, #220).
;
; Every vector funnels into isr_common, which saves the general registers into a
; trap_frame_t (see include/core/cpu.h) on the current stack and calls
; trap_dispatch(frame). The dispatcher may rewrite the frame in place (that is a
; context switch: the next process's registers go into the frame and CR3 is
; reloaded), then isr_common restores whatever the frame holds and iretqs.
;
; The debug monitor runs on the boot stack and treats the recorded machine as a
; coroutine: machine_enter() saves the monitor's callee-saved registers and
; irets into user mode from a frame at the top of the trap stack (TSS.rsp0);
; machine_exit(), called from trap_dispatch when the machine stops, drops the
; trap stack and returns from machine_enter() on the monitor stack.

global isr_stub_table
global trap_return
global machine_enter
global machine_exit
extern trap_dispatch

section .text
bits 64

%macro ISR_NOERR 1
isr_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_%1:
    push qword %1
    jmp isr_common
%endmacro

; CPU exceptions 0-31: vectors 8, 10-14, 17, 21, 29, 30 push an error code.
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
; Legacy PIC IRQs, remapped to 32-47.
%assign v 32
%rep 16
ISR_NOERR v
%assign v v + 1
%endrep
; System call gate (int 0x80, DPL 3).
ISR_NOERR 128

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp
    call trap_dispatch
trap_restore:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                     ; vector + error code
    iretq

; void trap_return(trap_frame_t* frame): restore `frame` and iretq to it.
trap_return:
    mov rsp, rdi
    jmp trap_restore

; void machine_enter(trap_frame_t* frame): run the machine from `frame` (which
; the caller placed at the top of the trap stack). Returns when machine_exit()
; is called.
machine_enter:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [monitor_rsp], rsp
    mov rsp, rdi
    jmp trap_restore

; void machine_exit(void): abandon the trap stack and return from the pending
; machine_enter() on the monitor stack.
machine_exit:
    mov rsp, [monitor_rsp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

section .rodata
align 8
; Stub addresses for vectors 0-47, then 128 (index 48).
isr_stub_table:
%assign v 0
%rep 48
    dq isr_ %+ v
%assign v v + 1
%endrep
    dq isr_128

section .bss
align 8
monitor_rsp: resq 1

section .note.GNU-stack noalloc noexec nowrite progbits
