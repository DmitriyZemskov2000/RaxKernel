#ifndef WEBOS_DEVFS_H
#define WEBOS_DEVFS_H

#include "vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

vnode_t* devfs_get(const char* name);
vnode_t* devfs_console(void);

#ifdef __cplusplus
}
#endif

#endif
