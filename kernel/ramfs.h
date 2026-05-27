#ifndef WEBOS_RAMFS_H
#define WEBOS_RAMFS_H

#include "vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

vnode_t* ramfs_init(void);
vnode_t* ramfs_root(void);
vnode_t* ramfs_create_file(vnode_t* dir, const char* name);
vnode_t* ramfs_create_dir(vnode_t* parent, const char* name);
vnode_t* ramfs_mount_at(vnode_t* parent, const char* name, vnode_t* mounted_root);
int      ramfs_unlink(vnode_t* dir, const char* name);

#ifdef __cplusplus
}
#endif

#endif
