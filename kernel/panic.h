#ifndef WEBOS_PANIC_H
#define WEBOS_PANIC_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((noreturn))
static inline void panic(const char* msg) {
    printf("\n\n*** KERNEL PANIC: %s ***\n", msg);
    for (;;) __asm__ volatile("cli; hlt");
}

#ifdef __cplusplus
}
#endif

#endif
