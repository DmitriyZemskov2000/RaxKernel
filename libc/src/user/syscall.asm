; ============================================================
; syscall.asm — обёртки для syscall'ов из userspace.
; ============================================================
; ABI userspace-сисколла:
;   RAX = номер сисколла
;   RDI, RSI, RDX, R10, R8, R9 = аргументы 1..6
;   результат в RAX (отрицательные = -errno)
;
; С точки зрения C ABI (SysV) 4-й аргумент идёт в RCX, но RCX
; используется syscall'ом для сохранения user RIP. Поэтому
; правильный С-прототип для 4+ аргументов требует, чтобы мы
; ПЕРЕД syscall'ом переложили RCX → R10.
; ============================================================

bits 64
section .text

; long syscall0(long num)
global syscall0
syscall0:
    mov rax, rdi
    syscall
    ret

; long syscall1(long num, long a1)
global syscall1
syscall1:
    mov rax, rdi
    mov rdi, rsi
    syscall
    ret

; long syscall2(long num, long a1, long a2)
global syscall2
syscall2:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    syscall
    ret

; long syscall3(long num, long a1, long a2, long a3)
global syscall3
syscall3:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    syscall
    ret

; long syscall4(long num, long a1, long a2, long a3, long a4)
global syscall4
syscall4:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    mov r10, r8                ; 4-й arg: SysV даёт R8, syscall ABI ждёт R10
    syscall
    ret

; long syscall6(long num, long a1, long a2, long a3, long a4, long a5, long a6)
global syscall6
syscall6:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    mov r10, r8
    mov r8,  r9
    mov r9,  [rsp + 8]         ; 7-й аргумент C — но он передаётся через стек
    syscall
    ret
