#ifndef WEBOS_VFS_H
#define WEBOS_VFS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef i64 off_t;

#define VNODE_FILE   1
#define VNODE_DIR    2
#define VNODE_CHAR   3      /* character device */

struct vnode;
struct vnode_ops {
    /* read/write по offset (без изменения курсора — это работа VFS) */
    ssize_t (*read)(struct vnode* v, void* buf, size_t n, off_t off);
    ssize_t (*write)(struct vnode* v, const void* buf, size_t n, off_t off);
    /* lookup ребёнка по имени; NULL если нет */
    struct vnode* (*lookup)(struct vnode* dir, const char* name);
    /* readdir: записать имя N-го ребёнка в name_out; вернуть длину или -1 */
    int (*readdir)(struct vnode* dir, size_t idx, char* name_out, size_t name_max);
    /* создать файл-ребёнка с данным именем; NULL если не поддерживается */
    struct vnode* (*create)(struct vnode* dir, const char* name);
    /* освободить vnode (для FS, создающих vnode динамически, напр. ext2) */
    void (*release)(struct vnode* v);
};

typedef struct vnode {
    int type;                       /* VNODE_FILE / DIR / CHAR */
    u64 size;
    const struct vnode_ops* ops;
    void* priv;                     /* backend-specific data */
} vnode_t;

struct vfs_stat {
    u64 size;
    int type;
};

void     vfs_set_root(vnode_t* root);
vnode_t* vfs_get_root(void);
vnode_t* vfs_lookup(const char* path);
void vnode_release(vnode_t* v);

int      vfs_open(const char* path, int flags);
int      vfs_close(int fd);
ssize_t  vfs_read(int fd, void* buf, size_t count);
ssize_t  vfs_write(int fd, const void* buf, size_t count);
off_t    vfs_lseek(int fd, off_t off, int whence);
int      vfs_stat(const char* path, struct vfs_stat* st);
int      vfs_readdir(vnode_t* dir, size_t idx, char* name_out, size_t name_max);

void     vfs_init_stdio(vnode_t* console);
vnode_t* vfs_get_vnode(int fd);

int      vfs_install_fd(vnode_t* v);
int      vfs_dup(int oldfd);
int      vfs_dup2(int oldfd, int newfd);

#ifdef __cplusplus
}
#endif

#endif
