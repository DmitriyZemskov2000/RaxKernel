; ============================================================
; switch.asm — ядерное переключение контекста между задачами.
; ============================================================
; Прототип:
;   void switch_context(uint64_t** old_rsp_ptr, uint64_t* new_rsp,
;                       void* old_fxsave_area, void* new_fxsave_area);
; ABI System V AMD64:
;   RDI = old_rsp_ptr
;   RSI = new_rsp
;   RDX = old_fxsave_area (16-байт выровненный, 512 байт)
;   RCX = new_fxsave_area
;
; Сохраняем:
;   - callee-saved регистры на стек
;   - FPU/SSE состояние через fxsave в свой буфер задачи
; Восстанавливаем:
;   - FPU/SSE из буфера новой задачи (или fninit, если буфер ещё пуст)
;   - callee-saved со стека новой задачи
; ============================================================

bits 64
global switch_context

switch_context:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp                  ; сохранили старый RSP

    ; Сохраняем FPU/SSE state старой задачи.
    ; Буфер задачи 16-байт выровнен (выделяется через aligned_alloc-style).
    fxsave [rdx]

    ; Переключаемся на новый стек
    mov rsp, rsi

    ; Восстанавливаем FPU/SSE state новой задачи
    fxrstor [rcx]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret
