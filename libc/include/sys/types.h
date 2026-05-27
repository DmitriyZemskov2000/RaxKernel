#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int    pid_t;
typedef int    uid_t;
typedef int    gid_t;
typedef long   off_t;
typedef long   ssize_t;
typedef long   time_t;
typedef long   suseconds_t;
typedef long   clock_t;
typedef int    mode_t;
typedef long   blkcnt_t;
typedef long   blksize_t;
typedef int    dev_t;
typedef long   ino_t;
typedef int    nlink_t;
typedef long   id_t;
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

#ifdef __cplusplus
}
#endif

#endif
