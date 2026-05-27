/*
 * idt.c — таблица дескрипторов прерываний (IDT).
 *
 * В x86 при любом исключении (деление на 0, page fault, …) или
 * аппаратном прерывании (IRQ от таймера, клавиатуры) CPU смотрит
 * в IDT по номеру прерывания и прыгает на указанный там обработчик.
 *
 * Без IDT любое исключение → triple fault → ребут. Поэтому минимум,
 * который должен быть в ядре сразу, — это рабочая IDT с заглушками,
 * которые хотя бы печатают, что произошло.
 *
 * Сами обработчики (isr_stubX) живут в idt_stubs.asm: они сохраняют
 * контекст, кладут номер прерывания и зовут общий обработчик на C.
 */

#include "types.h"
#include "io.h"
#include <stdio.h>
#include "panic.h"
#include "idt.h"

#define IDT_ENTRIES 256

typedef struct PACKED {
    u16 offset_low;
    u16 selector;
    u8  ist;            /* IST index, 0 = не использовать */
    u8  type_attr;      /* type + DPL + present */
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
} idt_entry_t;

typedef struct PACKED {
    u16 limit;
    u64 base;
} idtr_t;

static idt_entry_t idt[IDT_ENTRIES];
static idtr_t      idtr;

/* Объявления стабов из idt_stubs.asm */
extern void isr_stub_0(void);  extern void isr_stub_1(void);
extern void isr_stub_2(void);  extern void isr_stub_3(void);
extern void isr_stub_4(void);  extern void isr_stub_5(void);
extern void isr_stub_6(void);  extern void isr_stub_7(void);
extern void isr_stub_8(void);  extern void isr_stub_9(void);
extern void isr_stub_10(void); extern void isr_stub_11(void);
extern void isr_stub_12(void); extern void isr_stub_13(void);
extern void isr_stub_14(void); extern void isr_stub_15(void);
extern void isr_stub_16(void); extern void isr_stub_17(void);
extern void isr_stub_18(void); extern void isr_stub_19(void);
extern void isr_stub_20(void); extern void isr_stub_21(void);
extern void isr_stub_22(void); extern void isr_stub_23(void);
extern void isr_stub_24(void); extern void isr_stub_25(void);
extern void isr_stub_26(void); extern void isr_stub_27(void);
extern void isr_stub_28(void); extern void isr_stub_29(void);
extern void isr_stub_30(void); extern void isr_stub_31(void);

static void idt_set(u8 vec, void (*handler)(void), u8 ist, u8 type_attr) {
    u64 addr = (u64)handler;
    idt[vec].offset_low  = addr & 0xFFFF;
    idt[vec].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vec].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vec].selector    = 0x08;        /* код-сегмент из нашего GDT */
    idt[vec].ist         = ist;
    idt[vec].type_attr   = type_attr;   /* 0x8E: present, ring 0, interrupt gate */
    idt[vec].zero        = 0;
}

static const char* exception_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coproc segment overrun", "Invalid TSS", "Segment not present",
    "Stack segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 FPU error", "Alignment check", "Machine check", "SIMD FP exception",
    "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection", "VMM communication", "Security exception", "Reserved",
};

/* Мини-вывод числа через serial — не зависит от libc/SSE. */
extern void serial_putc(char c);
static void serial_puts(const char* s) { while (*s) serial_putc(*s++); }
static void serial_puthex64(u64 v) {
    char buf[17]; buf[16] = '\0';
    for (int i = 15; i >= 0; i--) {
        u8 nib = v & 0xF; v >>= 4;
        buf[i] = nib < 10 ? '0' + nib : 'a' + nib - 10;
    }
    serial_puts(buf);
}

/*
 * Общий обработчик. Вызывается из ASM-стаба после сохранения регистров.
 * Аргументом получает номер прерывания и код ошибки (если был).
 *
 * Используем ТОЛЬКО safe-вывод через serial, потому что printf
 * может сам уронить нас (он использует SSE; если #UD из-за SSE,
 * рекурсия), а нам важно увидеть номер исключения.
 */
void isr_common_handler(u64 vec, u64 err_code, u64 rip, u64 user_rsp) {
    serial_puts("\n*** EXCEPTION 0x");
    serial_puthex64(vec);
    serial_puts(" err=0x");
    serial_puthex64(err_code);
    serial_puts(" rip=0x");
    serial_puthex64(rip);
    serial_puts(" rsp=0x");
    serial_puthex64(user_rsp);
    if (vec < 32) {
        serial_puts(" (");
        serial_puts(exception_names[vec]);
        serial_puts(")");
    }
    serial_puts(" ***\n");

    if (vec == 14) {
        u64 cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        serial_puts("    CR2=0x");
        serial_puthex64(cr2);
        serial_puts("\n");
    }

    /* Зависнем БЕЗ panic'а (panic зовёт printf, который SSE). */
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

/* ---------- IRQ ---------- */

extern void irq_stub_0(void);  extern void irq_stub_1(void);
extern void irq_stub_2(void);  extern void irq_stub_3(void);
extern void irq_stub_4(void);  extern void irq_stub_5(void);
extern void irq_stub_6(void);  extern void irq_stub_7(void);
extern void irq_stub_8(void);  extern void irq_stub_9(void);
extern void irq_stub_10(void); extern void irq_stub_11(void);
extern void irq_stub_12(void); extern void irq_stub_13(void);
extern void irq_stub_14(void); extern void irq_stub_15(void);

#include "pic.h"
#include "sched.h"

/* Таблица обработчиков, заполняемая через irq_register. */
typedef void (*irq_fn_t)(u8 irq);
static irq_fn_t irq_handlers[16] = {0};

void irq_register(u8 irq, irq_fn_t fn) {
    if (irq < 16) irq_handlers[irq] = fn;
}

/* Вызывается из ASM-стаба IRQ. Аргумент — номер IRQ (0..15). */
void irq_handler(u64 irq) {
    if (irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq]((u8)irq);
    }
    /* EOI ДО переключения контекста: sched_tick может уйти в другую
       задачу через switch_context и никогда не вернуться сюда. Если
       мы не отправим EOI, PIC посчитает IRQ незавершённым и заблокирует
       следующие IRQ навсегда. */
    pic_eoi((u8)irq);
    if (irq == 0) {
        sched_tick();
    }
}

void idt_init(void) {
    void (*exc_stubs[32])(void) = {
        isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3,
        isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7,
        isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11,
        isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15,
        isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19,
        isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23,
        isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27,
        isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31,
    };
    for (int i = 0; i < 32; i++) {
        idt_set((u8)i, exc_stubs[i], 0, 0x8E);
    }

    void (*irq_stubs[16])(void) = {
        irq_stub_0,  irq_stub_1,  irq_stub_2,  irq_stub_3,
        irq_stub_4,  irq_stub_5,  irq_stub_6,  irq_stub_7,
        irq_stub_8,  irq_stub_9,  irq_stub_10, irq_stub_11,
        irq_stub_12, irq_stub_13, irq_stub_14, irq_stub_15,
    };
    for (int i = 0; i < 16; i++) {
        /* После pic_remap векторы IRQ начинаются с 0x20. */
        idt_set((u8)(0x20 + i), irq_stubs[i], 0, 0x8E);
    }

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (u64)&idt;
    __asm__ volatile("lidt %0" : : "m"(idtr));
}
