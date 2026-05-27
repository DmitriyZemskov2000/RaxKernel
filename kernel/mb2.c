/*
 * mb2.c — поиск Multiboot2 module tag'ов.
 *
 * GRUB загружает initrd как Multiboot2 module и кладёт описатель в info:
 *   type = 3 (MODULE)
 *   mod_start, mod_end (физ. адреса)
 *   string (имя — обычно cmdline в grub.cfg)
 */

#include "types.h"
#include <string.h>
#include "mb2.h"

typedef struct PACKED {
    u32 total_size;
    u32 reserved;
} mb2_info_t;

typedef struct PACKED {
    u32 type;
    u32 size;
} mb2_tag_t;

typedef struct PACKED {
    u32 type;
    u32 size;
    u32 mod_start;
    u32 mod_end;
    char string[0];
} mb2_module_t;

#define MB2_TAG_MODULE 3
#define MB2_TAG_END    0

int mb2_find_module(void* info, const char* name_hint,
                    u64* out_start, u64* out_end) {
    mb2_info_t* hdr = (mb2_info_t*)info;
    u8* p   = (u8*)info + 8;
    u8* end = (u8*)info + hdr->total_size;

    while (p < end) {
        mb2_tag_t* tag = (mb2_tag_t*)p;
        if (tag->type == MB2_TAG_END) break;

        if (tag->type == MB2_TAG_MODULE) {
            mb2_module_t* m = (mb2_module_t*)tag;
            /* Если имя задано, ищем подстроку; иначе первый module */
            int match = 1;
            if (name_hint) {
                match = 0;
                const char* s = m->string;
                for (const char* q = s; *q; q++) {
                    /* substring match */
                    const char* a = q;
                    const char* b = name_hint;
                    while (*a && *b && *a == *b) { a++; b++; }
                    if (!*b) { match = 1; break; }
                }
            }
            if (match) {
                if (out_start) *out_start = m->mod_start;
                if (out_end)   *out_end   = m->mod_end;
                return 1;
            }
        }

        /* выравниваем размер до 8 байт */
        p += (tag->size + 7) & ~7ULL;
    }
    return 0;
}

/* ---- Multiboot2 framebuffer tag (type=8) ---- */
typedef struct PACKED {
    u32 type;
    u32 size;
    u64 fb_addr;       /* физ. адрес фреймбуфера */
    u32 fb_pitch;      /* байт на строку */
    u32 fb_width;
    u32 fb_height;
    u8  fb_bpp;        /* бит на пиксель */
    u8  fb_type;       /* 1 = RGB */
    u16 reserved;
    /* далее цветовая палитра / маски */
} mb2_framebuffer_t;

#define MB2_TAG_FRAMEBUFFER 8

int mb2_find_framebuffer(void* info, u64* addr, u32* pitch,
                         u32* width, u32* height, u8* bpp) {
    mb2_info_t* hdr = (mb2_info_t*)info;
    u8* p   = (u8*)info + 8;
    u8* end = (u8*)info + hdr->total_size;

    while (p < end) {
        mb2_tag_t* tag = (mb2_tag_t*)p;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_FRAMEBUFFER) {
            mb2_framebuffer_t* fb = (mb2_framebuffer_t*)tag;
            if (addr)   *addr   = fb->fb_addr;
            if (pitch)  *pitch  = fb->fb_pitch;
            if (width)  *width  = fb->fb_width;
            if (height) *height = fb->fb_height;
            if (bpp)    *bpp    = fb->fb_bpp;
            return 1;
        }
        /* tag'и выровнены по 8 байт */
        p += (tag->size + 7) & ~7;
    }
    return 0;
}
