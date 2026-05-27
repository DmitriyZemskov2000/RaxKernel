#ifndef WEBOS_GDT_H
#define WEBOS_GDT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Селекторы, должны совпадать с layout'ом из gdt.c */
#define GDT_KERNEL_CS  0x08
#define GDT_KERNEL_DS  0x10
#define GDT_USER_DS    (0x18 | 3)
#define GDT_USER_CS    (0x20 | 3)

void gdt_init(void);
void tss_set_kernel_stack(u64 rsp);
u64  tss_get_default_kernel_stack(void);

#ifdef __cplusplus
}
#endif

#endif
