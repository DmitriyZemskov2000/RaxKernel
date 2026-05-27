#ifndef WEBOS_E1000_H
#define WEBOS_E1000_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

int  e1000_init(void);
int  e1000_is_ready(void);
void e1000_get_mac(u8* out);          /* out[6] */
int  e1000_send(const void* data, u16 len);
int  e1000_receive(void* buf, u16 maxlen);   /* 0 = нет пакета */

#ifdef __cplusplus
}
#endif

#endif
