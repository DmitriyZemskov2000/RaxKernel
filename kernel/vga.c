/*
 * vga.c — текстовый драйвер VGA (режим 80x25).
 *
 * VGA в text mode маппит видеопамять на физический адрес 0xB8000.
 * Каждый символ — 2 байта: код символа + атрибут (цвет фона/текста).
 * Поскольку первый 1 GiB у нас identity-mapped, можно просто писать
 * в 0xB8000 как в обычную память.
 */

#include "types.h"
#include "io.h"
#include "vga.h"

#define VGA_BUFFER ((volatile u16*)0xB8000)
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static size_t cursor_x = 0;
static size_t cursor_y = 0;
static u8 current_color = 0x0F;  /* белый на чёрном */

static inline u16 vga_entry(char c, u8 color) {
    return (u16)c | ((u16)color << 8);
}

void vga_set_color(u8 fg, u8 bg) {
    current_color = (u8)((bg << 4) | (fg & 0x0F));
}

void vga_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            VGA_BUFFER[y * VGA_WIDTH + x] = vga_entry(' ', current_color);
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

/* Скроллим экран вверх на одну строку, когда упёрлись в низ. */
static void vga_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            VGA_BUFFER[(y - 1) * VGA_WIDTH + x] = VGA_BUFFER[y * VGA_WIDTH + x];
        }
    }
    /* Очищаем последнюю строку */
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        VGA_BUFFER[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', current_color);
    }
    cursor_y = VGA_HEIGHT - 1;
}

void vga_putc(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~7;
    } else if (c == '\b') {
        if (cursor_x > 0) cursor_x--;
        VGA_BUFFER[cursor_y * VGA_WIDTH + cursor_x] = vga_entry(' ', current_color);
    } else {
        VGA_BUFFER[cursor_y * VGA_WIDTH + cursor_x] = vga_entry(c, current_color);
        cursor_x++;
    }

    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }
    if (cursor_y >= VGA_HEIGHT) {
        vga_scroll();
    }
}

void vga_write(const char* s) {
    while (*s) vga_putc(*s++);
}

void vga_init(void) {
    vga_set_color(0x0F, 0x00);
    vga_clear();
}
