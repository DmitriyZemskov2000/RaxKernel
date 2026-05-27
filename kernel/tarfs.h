#ifndef WEBOS_TARFS_H
#define WEBOS_TARFS_H

#include "vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Парсит USTAR-архив, строит дерево vnode'ов. Возвращает корневой vnode. */
vnode_t* tarfs_init(const void* archive, size_t size);

/* Добавить device-vnode в корень (напр. для /null, /zero, /console) */
void tarfs_add_device(const char* name, vnode_t* dev);

#ifdef __cplusplus
}
#endif

#endif
