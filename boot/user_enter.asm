; ============================================================
; user_enter.asm — первый переход в ring 3.
; ============================================================
; Два API:
;
; 1) void enter_userspace(uint64_t rip, uint64_t rsp);
;    Прямой переход — строит iret frame и делает iretq.
;    Используется когда мы УЖЕ в kernel task'е и хотим уйти в ring3.
;
; 2) user_iret_trampoline (без параметров).
;    Вызывается через switch_context.ret в первой шедулинговой
;    активации user-задачи. На момент вызова RSP указывает на
;    уже подготовленный iret frame (см. task_create_user в sched.c).
;    Просто делает iretq.
; ============================================================

bits 64
section .text

global enter_userspace
enter_userspace:
    xor rax, rax
    xor rcx, rcx
    xor rdx, rdx
    xor rbx, rbx
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    push qword 0x1B                     ; SS = USER_DS|3
    push rsi                            ; RSP
    push qword 0x202                    ; RFLAGS: IF=1
    push qword 0x23                     ; CS = USER_CS|3
    push rdi                            ; RIP

    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    ; НЕ mov gs, ax — в long mode это обнуляет MSR_GS_BASE

    iretq

; ----- Для первой активации user-задачи через планировщик -----
; Стек уже содержит готовый iret frame — switch_context'у мы дали
; адрес этой функции как "return address", и pop'ы регистров уже
; были сделаны.

global user_iret_trampoline
user_iret_trampoline:
    add rsp, 8
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    swapgs
    iretq

; ----- fork_iret_trampoline: восстанавливает все GP регистры из стека -----
; Layout стека на момент входа в trampoline (после switch_context.ret):
;   [RSP+0]   = padding qword
;   [RSP+8]   = R15 (parent value)
;   [RSP+16]  = R14
;   [RSP+24]  = R13
;   [RSP+32]  = R12
;   [RSP+40]  = R11
;   [RSP+48]  = R10
;   [RSP+56]  = R9
;   [RSP+64]  = R8
;   [RSP+72]  = RBP
;   [RSP+80]  = RDI
;   [RSP+88]  = RSI
;   [RSP+96]  = RDX
;   [RSP+104] = RCX
;   [RSP+112] = RBX
;   [RSP+120] = RAX (=0 для child)
;   [RSP+128] = RIP
;   [RSP+136] = CS
;   [RSP+144] = RFLAGS
;   [RSP+152] = RSP user
;   [RSP+160] = SS
global fork_iret_trampoline
fork_iret_trampoline:
    add rsp, 8              ; снимаем padding
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
    pop rax                 ; RAX = 0 для child

    mov dx, 0x1B
    mov ds, dx
    mov es, dx
    ; НЕ трогаем FS: загрузка селектора сбросила бы FS base (TLS),
    ; который мы только что восстановили через IA32_FS_BASE MSR в do_switch.
    ; musl/glibc держат thread pointer в FS base.
    swapgs
    iretq

; ============================================================
; execve_to_user — переход в новую программу после execve.
; ============================================================
; Не возвращается. Принимает:
;   RDI = new user RIP
;   RSI = new user RSP
;
; Прямой переход через iret-frame, обходя normal syscall sysret.
; Текущий kernel stack будет переиспользован при следующем syscall'е.
;
; В этот момент GS = percpu (мы внутри syscall_entry context).
; Поэтому swapgs нужен перед iretq.
; ============================================================

global execve_to_user
execve_to_user:
    cli                          ; safety на момент перестроения

    ; Перезагрузим percpu.kernel_rsp на top нашей kstack
    ; (потому что sysret cleanup пропустили, а syscall_entry
    ; всё-равно начнёт с mov rsp, [gs:8] в следующий раз).
    ; Делать ничего не надо: kstack_top остался в [gs:8].

    ; Строим iret frame на kernel stack
    push qword 0x1B              ; SS = USER_DS|3
    push rsi                     ; RSP user
    push qword 0x202             ; RFLAGS: IF=1
    push qword 0x23              ; CS = USER_CS|3
    push rdi                     ; RIP user

    ; Обнуляем GP регистры для гигиены (новая программа не должна
    ; видеть kernel state).
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    xor rdi, rdi
    xor rsi, rsi

    swapgs                       ; GS = user
    iretq
