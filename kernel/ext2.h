#ifndef RAXOS_EXT2_H
#define RAXOS_EXT2_H

#include "vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

vnode_t* ext2_mount(void);
int      ext2_available(void);

#ifdef __cplusplus
}
#endif

#endif
