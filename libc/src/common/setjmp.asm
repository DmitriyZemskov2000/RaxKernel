; ============================================================
; setjmp.asm — нелокальный переход для x86_64 System V ABI.
; ============================================================
; jmp_buf layout (8 long, 64 байта):
;   [0] = rbx
;   [1] = rbp
;   [2] = r12
;   [3] = r13
;   [4] = r14
;   [5] = r15
;   [6] = rsp (after return)
;   [7] = rip (return address of setjmp caller)
;
; setjmp возвращает 0; longjmp возвращает val (с 0 → 1).
; ============================================================

bits 64
section .text

global setjmp
setjmp:
    ; RDI = env
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15

    ; RSP, который будет ПОСЛЕ ret — это [rsp] + 8 (поскольку ret поп'ит адрес)
    lea rax, [rsp + 8]
    mov [rdi + 48], rax

    mov rax, [rsp]              ; return address — RIP вызвавшего setjmp
    mov [rdi + 56], rax

    xor eax, eax                ; return 0
    ret

global longjmp
longjmp:
    ; RDI = env, RSI = val
    mov eax, esi
    test eax, eax
    jnz .nonzero
    mov eax, 1                  ; C99: longjmp(env, 0) даёт setjmp вернуть 1
.nonzero:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]

    ; Перезаписываем адрес возврата на сохранённый RIP,
    ; затем ret уходит туда.
    push qword [rdi + 56]
    ret
