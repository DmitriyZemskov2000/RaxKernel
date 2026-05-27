#ifndef WEBOS_VIRTIO_BLK_H
#define WEBOS_VIRTIO_BLK_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

int virtio_blk_init(void);
int virtio_blk_read(u64 sector, void* buf);   /* buf >= 512 байт */
int virtio_blk_write(u64 sector, void* buf);
int virtio_blk_available(void);

#ifdef __cplusplus
}
#endif

#endif
