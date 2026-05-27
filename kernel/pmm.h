#ifndef WEBOS_PMM_H
#define WEBOS_PMM_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void  pmm_init(void* multiboot_info);
void  pmm_reserve_range(u64 phys_start, u64 phys_end);
void* pmm_alloc_page(void);
void  pmm_free_page(void* addr);
u64   pmm_free_count(void);
u64   pmm_total_count(void);

#ifdef __cplusplus
}
#endif

#endif
