#ifndef WEBOS_HEAP_H
#define WEBOS_HEAP_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void  heap_init(void);
void* kmalloc(size_t size);
void* krealloc(void* ptr, size_t new_size);
void  kfree(void* ptr);
void  heap_dump_stats(void);

#ifdef __cplusplus
}
#endif

#endif
