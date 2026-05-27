/*
 * fb.c — драйвер линейного фреймбуфера (graphics framebuffer).
 *
 * GRUB по запросу из multiboot2 header'а ставит графический режим и
 * передаёт адрес/геометрию через framebuffer-tag. Здесь — рисование
 * пикселей, прямоугольников и текста встроенным шрифтом 8x8.
 *
 * Формат пикселя: предполагаем 32 bpp BGRA/RGBA (QEMU stdvga даёт
 * 0x00RRGGBB little-endian, т.е. байты B,G,R,X).
 */
#include "types.h"
#include <string.h>
#include <stdio.h>
#include "fb.h"
#include "vmm.h"
#include "mb2.h"
#include "font8x8.h"

static u8*  fb_mem    = 0;        /* виртуальный адрес фреймбуфера */
static u64  fb_phys   = 0;
static u32  fb_pitch  = 0;        /* байт на строку */
static u32  fb_width  = 0;
static u32  fb_height = 0;
static u8   fb_bpp    = 0;
static int  fb_ready  = 0;

int fb_init(void* multiboot_info) {
    u64 addr; u32 pitch, w, h; u8 bpp;
    if (!mb2_find_framebuffer(multiboot_info, &addr, &pitch, &w, &h, &bpp)) {
        printf("[fb] no framebuffer tag — текстовый режим\n");
        return -1;
    }
    if (bpp != 32) {
        printf("[fb] bpp=%d (ожидали 32) — пропускаем\n", bpp);
        return -1;
    }

    fb_phys   = addr;
    fb_pitch  = pitch;
    fb_width  = w;
    fb_height = h;
    fb_bpp    = bpp;

    /* Маппим фреймбуфер в адресное пространство ядра.
       Размер = pitch * height, округляем до страниц. */
    u64 size = (u64)pitch * h;
    u64 pages = (size + 4095) / 4096;
    /* Линейный маппинг по фиксированному виртуальному адресу. */
    u64 vbase = 0xFFFFFFFFA0000000ULL;   /* регион под framebuffer */
    for (u64 i = 0; i < pages; i++) {
        vmm_map(vbase + i * 4096, fb_phys + i * 4096, VMM_WRITABLE);
    }
    fb_mem = (u8*)vbase;
    fb_ready = 1;

    printf("[fb] %ux%u %ubpp pitch=%u phys=0x%lx -> 0x%lx (%lu KiB)\n",
           w, h, bpp, pitch, fb_phys, vbase, size / 1024);
    return 0;
}

int  fb_is_ready(void) { return fb_ready; }
u32  fb_get_width(void)  { return fb_width; }
u32  fb_get_height(void) { return fb_height; }

void fb_putpixel(u32 x, u32 y, u32 color) {
    if (!fb_ready || x >= fb_width || y >= fb_height) return;
    u32* p = (u32*)(fb_mem + (u64)y * fb_pitch + (u64)x * 4);
    *p = color;
}

void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    if (!fb_ready) return;
    for (u32 dy = 0; dy < h; dy++) {
        u32 yy = y + dy;
        if (yy >= fb_height) break;
        u32* row = (u32*)(fb_mem + (u64)yy * fb_pitch + (u64)x * 4);
        for (u32 dx = 0; dx < w; dx++) {
            if (x + dx >= fb_width) break;
            row[dx] = color;
        }
    }
}

void fb_clear(u32 color) {
    fb_fill_rect(0, 0, fb_width, fb_height, color);
}

/* Рисуем символ 8x8 встроенным шрифтом. */
void fb_draw_char(u32 x, u32 y, char c, u32 fg, u32 bg, int draw_bg) {
    if (!fb_ready) return;
    const u8* glyph = font8x8_basic[(u8)c & 0x7F];
    for (int row = 0; row < 8; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                fb_putpixel(x + col, y + row, fg);
            } else if (draw_bg) {
                fb_putpixel(x + col, y + row, bg);
            }
        }
    }
}

/* Рисуем строку. Поддержка \n. Возвращает финальную x-координату. */
void fb_draw_text(u32 x, u32 y, const char* s, u32 fg, u32 bg, int draw_bg) {
    u32 cx = x, cy = y;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; cy += 8; continue; }
        fb_draw_char(cx, cy, *s, fg, bg, draw_bg);
        cx += 8;
    }
}

/* Масштабированный символ (scale x scale пикселей на точку) — для
   крупного текста интерфейса. */
void fb_draw_char_scaled(u32 x, u32 y, char c, u32 fg, int scale) {
    if (!fb_ready) return;
    const u8* glyph = font8x8_basic[(u8)c & 0x7F];
    for (int row = 0; row < 8; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                fb_fill_rect(x + col * scale, y + row * scale,
                             scale, scale, fg);
            }
        }
    }
}

void fb_draw_text_scaled(u32 x, u32 y, const char* s, u32 fg, int scale) {
    u32 cx = x;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += 8 * scale; continue; }
        fb_draw_char_scaled(cx, y, *s, fg, scale);
        cx += 8 * scale;
    }
}

/* Быстрый вывод 32-битного буфера (w*h пикселей 0xXXRRGGBB) на экран. */
void fb_blit32(const u32* src, u32 w, u32 h) {
    if (!fb_ready) return;
    if (w > fb_width) w = fb_width;
    if (h > fb_height) h = fb_height;
    for (u32 y = 0; y < h; y++) {
        u32* dst = (u32*)(fb_mem + (u64)y * fb_pitch);
        const u32* s = src + (u64)y * fb_width;  /* backbuffer pitch = fb_width */
        for (u32 x = 0; x < w; x++) dst[x] = s[x];
    }
}

u32 fb_get_pitch(void) { return fb_pitch; }
u32 fb_get_bpp(void)   { return (u32)fb_bpp; }
