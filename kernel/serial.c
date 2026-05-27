/*
 * serial.c — драйвер UART 16550 (COM1).
 *
 * Зачем: VGA-экран хорошо для пользователя, но для разработки
 * удобнее логи в serial — QEMU умеет писать их прямо в stdout
 * хоста через -serial stdio. И в реальном железе serial — это
 * самый простой способ что-то увидеть, пока нет драйвера экрана.
 */

#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* отключаем прерывания */
    outb(COM1 + 3, 0x80);   /* enable DLAB — теперь порты 0/1 это делитель скорости */
    outb(COM1 + 0, 0x03);   /* делитель = 3 → 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 бит, без чётности, 1 стоп-бит; DLAB off */
    outb(COM1 + 2, 0xC7);   /* FIFO on, очистка, 14-байтный порог */
    outb(COM1 + 4, 0x0B);   /* IRQ enabled, RTS/DTR set */
}

static int serial_tx_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!serial_tx_empty()) { /* busy wait */ }
    outb(COM1, (u8)c);
}

void serial_write(const char* s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');  /* CRLF для нормальных терминалов */
        serial_putc(*s++);
    }
}
