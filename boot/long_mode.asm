; ============================================================
; WebOS — приземление в long mode и переход в kernel_main
; ============================================================
; После jmp gdt64.code:long_mode_start CPU уже в 64-битном режиме,
; но регистры сегментов всё ещё содержат мусор из protected mode.
; Обнуляем data-сегменты и вызываем kernel_main(multiboot_info_ptr).
; ============================================================

global long_mode_start
extern kernel_main

section .text
bits 64
long_mode_start:
    ; Обнуляем все data-сегменты — в long mode они игнорируются,
    ; но некоторые инструкции (например, IRET) могут на них смотреть.
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; ----- Включаем SSE -----
    ; В long mode SSE технически уже работает, но нужно правильно
    ; настроить CR0 и CR4, чтобы fxsave/fxrstor и SSE-инструкции
    ; не генерировали #UD/#NM.
    ; CR0.EM = 0 (не эмулируем FPU), CR0.MP = 1 (TS coordination)
    ; CR4.OSFXSR = 1 (поддерживаем fxsave/fxrstor)
    ; CR4.OSXMMEXCPT = 1 (поддерживаем XMM-исключения)
    mov rax, cr0
    and rax, ~(1 << 2)              ; clear EM
    or  rax, (1 << 1)               ; set MP
    mov cr0, rax

    mov rax, cr4
    or  rax, (1 << 9) | (1 << 10)   ; OSFXSR | OSXMMEXCPT
    mov cr4, rax

    ; Инициализируем FPU
    fninit

    ; RDI уже содержит указатель на Multiboot info (мы его положили
    ; в EDI в 32-битном _start, верхние биты были обнулены — long mode).
    call kernel_main

    ; Если ядро вернулось — это ошибка. Бесконечный hlt.
.hang:
    cli
    hlt
    jmp .hang
