#ifndef WEBOS_MB2_H
#define WEBOS_MB2_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ищет module-tag. Возвращает 1 если нашли. */
int mb2_find_module(void* info, const char* name_hint,
                    u64* out_start, u64* out_end);

/* Ищет framebuffer-tag. Возвращает 1 если нашли. */
int mb2_find_framebuffer(void* info, u64* addr, u32* pitch,
                         u32* width, u32* height, u8* bpp);

#ifdef __cplusplus
}
#endif

#endif
