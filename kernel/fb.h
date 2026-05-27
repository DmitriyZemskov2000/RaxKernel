#ifndef WEBOS_FB_H
#define WEBOS_FB_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Цвета в формате 0x00RRGGBB */
#define FB_BLACK   0x00000000
#define FB_WHITE   0x00FFFFFF
#define FB_RED     0x00FF0000
#define FB_GREEN   0x0000FF00
#define FB_BLUE    0x000000FF
#define FB_GRAY    0x00808080
#define FB_DARKGRAY 0x00303030
#define FB_CYAN    0x0000FFFF
#define FB_YELLOW  0x00FFFF00

int  fb_init(void* multiboot_info);
int  fb_is_ready(void);
u32  fb_get_width(void);
u32  fb_get_height(void);

void fb_putpixel(u32 x, u32 y, u32 color);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void fb_clear(u32 color);
void fb_draw_char(u32 x, u32 y, char c, u32 fg, u32 bg, int draw_bg);
void fb_draw_text(u32 x, u32 y, const char* s, u32 fg, u32 bg, int draw_bg);
void fb_draw_char_scaled(u32 x, u32 y, char c, u32 fg, int scale);
void fb_draw_text_scaled(u32 x, u32 y, const char* s, u32 fg, int scale);
void fb_blit32(const u32* src, u32 w, u32 h);

#ifdef __cplusplus
}
#endif

#endif
