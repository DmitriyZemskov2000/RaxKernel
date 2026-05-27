/*
 * pic.c — Programmable Interrupt Controller 8259 (legacy PIC).
 *
 * BIOS оставляет нам два каскадированных PIC'а с IRQ:
 *   master: IRQ0..7  → векторы 0x08..0x0F  (КОНФЛИКТУЕТ с CPU exceptions)
 *   slave:  IRQ8..15 → векторы 0x70..0x77
 *
 * Первое, что делаем — remap'аем их на 0x20..0x27 (master) и 0x28..0x2F (slave),
 * чтобы не пересекаться с исключениями. После этого IRQ N приедет
 * на IDT-вектор 0x20 + N.
 *
 * Современный путь — APIC/x2APIC, но PIC проще, и у нас один CPU. Когда
 * понадобится SMP — заменим на APIC, а API pic_eoi/pic_mask оставим.
 */

#include "types.h"
#include "io.h"
#include "pic.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define ICW1_ICW4  0x01
#define ICW1_INIT  0x10
#define ICW4_8086  0x01

#define PIC_EOI    0x20

void pic_remap(u8 offset_master, u8 offset_slave) {
    /* Сохраним маски, чтобы потом восстановить — обычно после init
       мы маскируем всё кроме нужных IRQ, но пусть здесь будет нейтрально. */
    u8 m1 = inb(PIC1_DATA);
    u8 m2 = inb(PIC2_DATA);

    /* ICW1: init + ICW4 needed */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: vector offset */
    outb(PIC1_DATA, offset_master); io_wait();
    outb(PIC2_DATA, offset_slave);  io_wait();

    /* ICW3: cascade. Master: бит 2 = есть slave на IRQ2. Slave: cascade id = 2. */
    outb(PIC1_DATA, 0b00000100); io_wait();
    outb(PIC2_DATA, 0b00000010); io_wait();

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    outb(PIC1_DATA, m1);
    outb(PIC2_DATA, m2);
}

void pic_mask_all(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_clear_mask(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    u8 mask = inb(port);
    mask &= ~(1u << irq);
    outb(port, mask);
}

void pic_set_mask(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    u8 mask = inb(port);
    mask |= (1u << irq);
    outb(port, mask);
}

void pic_eoi(u8 irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);   /* slave тоже надо подтвердить */
    outb(PIC1_CMD, PIC_EOI);
}
