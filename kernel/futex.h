#ifndef WEBOS_FUTEX_H
#define WEBOS_FUTEX_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

long sys_futex(int* uaddr, int op, int val, void* timeout);

#ifdef __cplusplus
}
#endif

#endif
