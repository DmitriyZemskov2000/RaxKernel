#ifndef WEBOS_ELF_H
#define WEBOS_ELF_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

int elf_load(const void* image, size_t image_size, u64* out_entry);

#ifdef __cplusplus
}
#endif

#endif
