; ============================================================
; pthread_trampoline.asm — старт нового thread'а.
; ============================================================
; Когда ядро через iretq запустит нас на child_stack, наверху
; стека (RSP) уже лежит:
;   [RSP+0]  = start_routine
;   [RSP+8]  = arg
;
; (положено pthread_create в libc — см. ниже.)
;
; Trampoline:
;   pop start, pop arg, выровнять стек, call start
;   при возврате — syscall SYS_EXIT
; ============================================================

bits 64
section .text

global __pthread_trampoline
__pthread_trampoline:
    pop rax            ; start_routine
    pop rdi            ; arg
    ; После 2 pop'ов RSP mod 16 == 0 (top был выровнен).
    ; ABI: перед call ожидается RSP mod 16 == 0. У нас как раз так —
    ; никакого выравнивания не нужно.
    call rax           ; start(arg) → результат в RAX
    mov rdi, rax
    mov rax, 60        ; SYS_EXIT
    syscall
.hang:
    jmp .hang
