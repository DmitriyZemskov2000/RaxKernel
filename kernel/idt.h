#ifndef WEBOS_IDT_H
#define WEBOS_IDT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void idt_init(void);
void irq_register(u8 irq, void (*fn)(u8 irq));

#ifdef __cplusplus
}
#endif

#endif
