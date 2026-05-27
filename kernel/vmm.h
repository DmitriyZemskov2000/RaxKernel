#ifndef WEBOS_VMM_H
#define WEBOS_VMM_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Флаги для vmm_map. Биты совпадают с PTE-флагами, чтобы можно
   было передавать комбинации напрямую. */
#define VMM_WRITABLE  (1ULL << 1)
#define VMM_USER      (1ULL << 2)
#define VMM_NX        (1ULL << 63)

void  vmm_init(void);
int   vmm_map(u64 vaddr, u64 paddr, u64 flags);
int   vmm_unmap(u64 vaddr);
u64   vmm_translate(u64 vaddr);     /* 0 если не замаплен */

u64   vmm_create_space(void);
u64   vmm_clone_address_space(void);
void  vmm_switch_space(u64 pml4_phys);
u64   vmm_current_cr3(void);
u64   vmm_kernel_cr3(void);

#ifdef __cplusplus
}
#endif

#endif
