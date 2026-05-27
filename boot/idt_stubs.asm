; ============================================================
; idt_stubs.asm — низкоуровневые входы в обработчики исключений
; ============================================================
; CPU при исключении кладёт на стек: SS, RSP, RFLAGS, CS, RIP
; Для исключений 8, 10..14, 17, 21, 29, 30 он также кладёт error code.
; Чтобы общий C-обработчик имел единый интерфейс, для остальных
; исключений мы сами кладём фиктивный код ошибки 0.
; ============================================================

bits 64

extern isr_common_handler

; Макрос: исключение БЕЗ кода ошибки. Кладём ноль вместо.
%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push qword 0          ; фиктивный error code
    push qword %1         ; номер вектора
    jmp isr_common
%endmacro

; Макрос: исключение С кодом ошибки. CPU уже его положил.
%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push qword %1
    jmp isr_common
%endmacro

ISR_NOERR 0   ; #DE — деление на ноль
ISR_NOERR 1   ; #DB
ISR_NOERR 2   ; NMI
ISR_NOERR 3   ; #BP
ISR_NOERR 4   ; #OF
ISR_NOERR 5   ; #BR
ISR_NOERR 6   ; #UD — invalid opcode
ISR_NOERR 7   ; #NM
ISR_ERR   8   ; #DF — double fault
ISR_NOERR 9
ISR_ERR   10  ; #TS
ISR_ERR   11  ; #NP
ISR_ERR   12  ; #SS
ISR_ERR   13  ; #GP — общая защитная
ISR_ERR   14  ; #PF — page fault
ISR_NOERR 15
ISR_NOERR 16  ; #MF
ISR_ERR   17  ; #AC
ISR_NOERR 18  ; #MC
ISR_NOERR 19  ; #XM
ISR_NOERR 20  ; #VE
ISR_ERR   21  ; #CP
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

; ----- Общая часть -----
; На стеке (сверху вниз):
;   [rsp + 0]  = vector
;   [rsp + 8]  = error code
;   [rsp + 16] = RIP
;   [rsp + 24] = CS
;   [rsp + 32] = RFLAGS
;   [rsp + 40] = RSP (старый)
;   [rsp + 48] = SS
;
; System V AMD64: первые три аргумента в RDI, RSI, RDX.
; isr_common_handler(vec, err, rip)
isr_common:
    pop rdi               ; vector
    pop rsi               ; error code
    mov rdx, [rsp]        ; RIP
    mov rcx, [rsp + 24]   ; user RSP (4-й аргумент)
    cld
    call isr_common_handler
.hang:
    cli
    hlt
    jmp .hang

; ============================================================
; IRQ стабы (векторы 0x20..0x2F)
; ============================================================
; При входе из ring 3 CPU делает switch на TSS.RSP0 и кладёт:
;   [RSP+ 0] = RIP, [RSP+ 8] = CS, [RSP+16] = RFLAGS,
;   [RSP+24] = RSP_user, [RSP+32] = SS_user
; При входе из ring 0 — то же самое (long mode всегда кладёт SS:RSP).
;
; Чтобы корректно работать с GS:
;   - Если came-from-user (CS & 3 == 3): swapgs в начале/конце
;   - Если came-from-kernel: НЕ swapgs (GS уже kernel'овский)
;
; После этого паттерна можно безопасно шедулить между user task'ами
; даже когда прерывание попало в user mode.
; ============================================================

extern irq_handler

%macro IRQ_STUB 1
global irq_stub_%1
irq_stub_%1:
    ; Сначала проверяем CPL до push'ев — CS лежит на [rsp+8]
    test qword [rsp+8], 3
    jz .from_kernel_%1
    swapgs
.from_kernel_%1:

    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; FPU/SSE state — fxsave требует 16-byte align'ed буфер.
    mov rbp, rsp
    and rsp, -16
    sub rsp, 512
    fxsave [rsp]

    cld
    mov rdi, %1
    call irq_handler

    fxrstor [rsp]
    mov rsp, rbp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    ; Перед iretq — обратный swapgs если возвращаемся в user.
    ; Тот же тест по сохранённому CS.
    test qword [rsp+8], 3
    jz .ret_kernel_%1
    swapgs
.ret_kernel_%1:
    iretq
%endmacro

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15
