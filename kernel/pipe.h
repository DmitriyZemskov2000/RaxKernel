#ifndef WEBOS_PIPE_H
#define WEBOS_PIPE_H

#include "vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

int pipe_create(vnode_t** out_read, vnode_t** out_write);

#ifdef __cplusplus
}
#endif

#endif
