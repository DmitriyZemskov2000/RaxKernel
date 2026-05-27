; ============================================================
; crt0.asm — entry point с поддержкой argv/envp.
; ============================================================
; Стек на момент входа в _start (SysV x86_64 ABI):
;   [rsp]              = argc
;   [rsp+8]            = argv[0] (pointer)
;   [rsp+16]           = argv[1]
;   ...
;   [rsp+8*argc]       = NULL  (terminator argv)
;   [rsp+8*(argc+1)]   = envp[0]
;   [rsp+8*(argc+2)]   = envp[1]
;   ...                = NULL  (terminator envp)
;   [next]             = auxv[]  (AT_NULL terminated)
;
; Что мы делаем:
;   1. argc → RDI (1-й аргумент main)
;   2. &argv[0] → RSI (2-й аргумент main)
;   3. &envp[0] → RDX (3-й аргумент main); сохраняем в environ глобал
;   4. call main
;   5. передаём RAX в _exit
; ============================================================

bits 64
section .text

extern main
extern _exit
extern __libc_init_environ

global _start
_start:
    ; RSP сейчас mod 16 == 0 (по ABI).
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    ; envp = &argv[argc+1]
    mov rax, rdi
    inc rax
    shl rax, 3
    lea rdx, [rsp + 8 + rax]    ; envp

    ; Сохраним environ в libc-глобал для getenv
    push rdi
    push rsi
    push rdx
    mov rdi, rdx                ; envp
    sub rsp, 8                  ; выравнивание
    call __libc_init_environ
    add rsp, 8
    pop rdx
    pop rsi
    pop rdi

    ; После 3 pop'ов RSP mod 16 == 0. ABI требует перед call mod 16 == 0.
    ; Никакого выравнивания не нужно.
    call main

    ; RAX = exit code
    mov edi, eax
    call _exit
.hang:
    jmp .hang
