; ============================================================
; syscall_entry.asm — точка входа SYSCALL.
; ============================================================
; SYSCALL: CPU сохраняет RIP→RCX, RFLAGS→R11, переходит на LSTAR.
; RSP не меняется CPU'ом — переключаем сами через GS:[8]=kernel_rsp.
;
; ABI пользователя:
;   RAX = syscall number
;   RDI, RSI, RDX, R10, R8, R9 = аргументы 1..6
;   возврат — в RAX
;
; LAYOUT kernel stack после всех push'ей (от RSP вверх):
;   [RSP+ 0]   = R15
;   [RSP+ 8]   = R14
;   [RSP+ 16]  = R13
;   [RSP+ 24]  = R12
;   [RSP+ 32]  = R11 (= user RFLAGS, CPU положил в R11 при syscall)
;   [RSP+ 40]  = R10
;   [RSP+ 48]  = R9
;   [RSP+ 56]  = R8
;   [RSP+ 64]  = RBP
;   [RSP+ 72]  = RDI
;   [RSP+ 80]  = RSI
;   [RSP+ 88]  = RDX
;   [RSP+ 96]  = RCX (= user RIP)
;   [RSP+104]  = RBX
;   [RSP+112]  = RAX (= syscall num originally)
;
; Этот фиксированный layout позволяет sys_fork() прочитать
; снапшот регистров parent'а из kernel stack.
; ============================================================

bits 64
section .text

extern syscall_dispatch

global syscall_entry
syscall_entry:
    swapgs
    mov [gs:0], rsp                     ; сохраняем user RSP
    mov rsp, [gs:8]                     ; берём kernel RSP

    ; Сохраняем ВСЕ регистры (порядок такой, что layout выше совпадает).
    push rax
    push rbx
    push rcx                            ; user RIP
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11                            ; user RFLAGS
    push r12
    push r13
    push r14
    push r15

    ; Сохраним указатель на snapshot регистров для sys_fork
    mov [gs:16], rsp

    ; Готовим аргументы для syscall_dispatch:
    ; dispatch(num, a1, a2, a3, a4, a5, a6)
    ; Linux ABI user→kernel: num=RAX, a1=RDI, a2=RSI, a3=RDX, a4=R10, a5=R8, a6=R9
    ; C ABI dispatch  : a1=RDI, a2=RSI, a3=RDX, a4=RCX, a5=R8, a6=R9, a7 на стеке

    push r9                             ; 7-й аргумент (a6) на стек
    mov r9, r8                          ; a5
    mov r8, r10                         ; a4
    mov rcx, rdx                        ; a3
    mov rdx, rsi                        ; a2
    mov rsi, rdi                        ; a1
    mov rdi, rax                        ; num

    cld
    ; RSP сейчас mod 16 == 0 (15 push * 8 + push r9 = 128 байт, base mod 16 == 0).
    ; Перед call надо mod 16 == 0. У нас уже так — НЕ добавляем sub.
    call syscall_dispatch

    add rsp, 8                          ; снимаем a6

    ; Восстанавливаем регистры. RAX (возвращаемое значение) НЕ перезаписываем!
    ; Поэтому пропускаем восстановление RAX в конце.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11                             ; user RFLAGS
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx                             ; user RIP
    pop rbx
    add rsp, 8                          ; пропускаем сохранённый RAX (мы хотим вернуть наше значение)

    mov rsp, [gs:0]                     ; возвращаем user RSP
    swapgs

    o64 sysret
