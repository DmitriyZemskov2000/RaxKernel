/*
 * io.h — инструкции in/out для x86 портов ввода-вывода.
 *
 * На x86 устройства типа PIC, PIT, serial UART, PS/2 контроллер
 * висят на отдельном адресном пространстве портов, доступ через
 * инструкции IN и OUT. Обернём их в inline-функции.
 */
#ifndef WEBOS_IO_H
#define WEBOS_IO_H

#include "types.h"

static inline void outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline u16 inw(u16 port) {
    u16 ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(u16 port, u32 val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline u32 inl(u16 port) {
    u32 ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * Короткая задержка после OUT — некоторые старые устройства
 * (типа PIC) не успевают принять данные. Запись в неиспользуемый
 * порт 0x80 — общепринятый трюк.
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif
