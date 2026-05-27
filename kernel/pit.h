#ifndef WEBOS_PIT_H
#define WEBOS_PIT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void pit_init(u32 hz);
void pit_tick_inc(void);
u64  pit_ticks(void);
u32  pit_frequency(void);
void pit_sleep_ms(u32 ms);

#ifdef __cplusplus
}
#endif

#endif
