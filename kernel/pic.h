#ifndef WEBOS_PIC_H
#define WEBOS_PIC_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIC_OFFSET_MASTER 0x20
#define PIC_OFFSET_SLAVE  0x28

void pic_remap(u8 offset_master, u8 offset_slave);
void pic_mask_all(void);
void pic_clear_mask(u8 irq);
void pic_set_mask(u8 irq);
void pic_eoi(u8 irq);

#ifdef __cplusplus
}
#endif

#endif
