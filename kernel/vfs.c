/*
 * vfs.c — Virtual File System layer.
 *
 * Идея: всё в системе (regular files, директории, устройства, pipe'ы)
 * представлено как vnode_t. Конкретный backend (devfs, tarfs, ramfs)
 * предоставляет ops-таблицу — функции read/write/lookup/readdir.
 *
 * Это минимальный VFS. В Linux это inode + dentry + super_block —
 * у нас пока всё в одной struct vnode. Когда будут несколько
 * файловых систем одновременно, добавлю monut-points.
 *
 * Mount-point модель: в этой итерации один корень "/" с одной FS.
 * Это достаточно для initrd с tarfs.
 *
 * File descriptor table — на уровне процесса/задачи. Пока у нас
 * один процесс (userspace task), храним глобально. Когда появится
 * fork(), вынесем в task_t.
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "vfs.h"

#define MAX_FDS 32

static vnode_t* fd_table[MAX_FDS];
static off_t    fd_offset[MAX_FDS];   /* offset для каждого открытого файла */

/* Корневой vnode. Назначается при монтировании FS. */
static vnode_t* vfs_root = NULL;

void vfs_set_root(vnode_t* root) {
    vfs_root = root;
}

vnode_t* vfs_get_root(void) {
    return vfs_root;
}

/* ---------- Lookup path ----------
 * Простой walker: разбивает путь по '/', для каждой компоненты
 * зовёт node->ops->lookup. Поддерживает абсолютные пути.
 */
vnode_t* vfs_lookup(const char* path) {
    if (!vfs_root) return NULL;
    if (!path || !*path) return NULL;

    /* Абсолютный путь обязательно */
    if (path[0] != '/') return NULL;
    path++;

    vnode_t* cur = vfs_root;
    char comp[64];

    while (*path) {
        /* Извлекаем компоненту до '/' или конца */
        size_t i = 0;
        while (*path && *path != '/' && i + 1 < sizeof(comp)) {
            comp[i++] = *path++;
        }
        comp[i] = '\0';
        if (*path == '/') path++;
        if (i == 0) continue;  /* "//" пропускаем */

        if (!cur->ops || !cur->ops->lookup) {
            if (cur != vfs_root) vnode_release(cur);
            return NULL;
        }
        vnode_t* next = cur->ops->lookup(cur, comp);
        /* Освобождаем промежуточный vnode (не root) перед спуском глубже */
        if (cur != vfs_root) vnode_release(cur);
        cur = next;
        if (!cur) return NULL;
    }
    return cur;
}

/* ---------- POSIX-style API ---------- */

int vfs_open(const char* path, int flags) {
    vnode_t* v = vfs_lookup(path);
    if (!v) {
        /* O_CREAT (0x40): создаём файл в родительской директории */
        if (!(flags & 0x40)) return -2;        /* -ENOENT */

        /* Разбиваем path на parent + name */
        const char* last = path;
        for (const char* p = path; *p; p++) if (*p == '/') last = p;
        if (last == path) return -2;
        char parent_path[128];
        size_t pl = (size_t)(last - path);
        if (pl == 0) { parent_path[0] = '/'; parent_path[1] = '\0'; }
        else {
            if (pl >= sizeof(parent_path)) return -36;
            memcpy(parent_path, path, pl);
            parent_path[pl] = '\0';
        }
        vnode_t* parent = vfs_lookup(parent_path);
        if (!parent || parent->type != VNODE_DIR) return -2;

        /* Если у родителя есть свой create (ext2) — используем его,
           иначе ramfs_create_file. */
        if (parent->ops && parent->ops->create) {
            v = parent->ops->create(parent, last + 1);
        } else {
            extern vnode_t* ramfs_create_file(vnode_t*, const char*);
            v = ramfs_create_file(parent, last + 1);
        }
        vnode_release(parent);
        if (!v) return -28;     /* -ENOSPC */
    }

    for (int fd = 3; fd < MAX_FDS; fd++) {
        if (!fd_table[fd]) {
            fd_table[fd] = v;
            fd_offset[fd] = 0;
            return fd;
        }
    }
    return -24;
}

int vfs_close(int fd) {
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd]) return -9;
    vnode_t* v = fd_table[fd];
    fd_table[fd] = NULL;
    fd_offset[fd] = 0;
    vnode_release(v);
    return 0;
}

ssize_t vfs_read(int fd, void* buf, size_t count) {
    if (fd < 0 || fd >= MAX_FDS) return -9;
    vnode_t* v = fd_table[fd];
    if (!v || !v->ops || !v->ops->read) return -9;

    off_t off = fd_offset[fd];
    ssize_t r = v->ops->read(v, buf, count, off);
    if (r > 0) fd_offset[fd] = off + r;
    return r;
}

ssize_t vfs_write(int fd, const void* buf, size_t count) {
    if (fd < 0 || fd >= MAX_FDS) return -9;
    vnode_t* v = fd_table[fd];
    if (!v || !v->ops || !v->ops->write) return -9;

    off_t off = fd_offset[fd];
    ssize_t r = v->ops->write(v, buf, count, off);
    if (r > 0) fd_offset[fd] = off + r;
    return r;
}

off_t vfs_lseek(int fd, off_t off, int whence) {
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd]) return -9;
    vnode_t* v = fd_table[fd];
    off_t newoff;
    switch (whence) {
        case 0: newoff = off; break;                           /* SEEK_SET */
        case 1: newoff = fd_offset[fd] + off; break;           /* SEEK_CUR */
        case 2: newoff = (off_t)v->size + off; break;          /* SEEK_END */
        default: return -22;
    }
    if (newoff < 0) return -22;
    fd_offset[fd] = newoff;
    return newoff;
}

int vfs_stat(const char* path, struct vfs_stat* st) {
    vnode_t* v = vfs_lookup(path);
    if (!v) return -2;
    st->size = v->size;
    st->type = v->type;
    return 0;
}

/* ---------- Установка stdin/stdout/stderr ---------- */
/* Эти fd используются userspace'ом, проверка fd<3 в close их защищает.
   sys_write игнорирует наш fd_table для 1/2 и пишет напрямую через kputs_raw
   (см. sys_impl.c — для простоты). В будущей итерации сделаем правильно
   через vfs_write на dev_console. */

void vfs_init_stdio(vnode_t* console) {
    fd_table[0] = console;
    fd_table[1] = console;
    fd_table[2] = console;
    fd_offset[0] = fd_offset[1] = fd_offset[2] = 0;
}

/* Возвращает vnode по дескриптору — нужно sys_write/read */
vnode_t* vfs_get_vnode(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return fd_table[fd];
}

/* ---------- readdir ---------- */

int vfs_readdir(vnode_t* dir, size_t idx, char* name_out, size_t name_max) {
    if (!dir || !dir->ops || !dir->ops->readdir) return -22;
    return dir->ops->readdir(dir, idx, name_out, name_max);
}

/* Найти свободный FD и положить туда vnode. Возвращает fd или -1. */
int vfs_install_fd(vnode_t* v) {
    for (int fd = 3; fd < MAX_FDS; fd++) {
        if (!fd_table[fd]) {
            fd_table[fd] = v;
            fd_offset[fd] = 0;
            return fd;
        }
    }
    return -24;
}

/* dup: дублирует FD в первый свободный slot */
int vfs_dup(int oldfd) {
    if (oldfd < 0 || oldfd >= MAX_FDS || !fd_table[oldfd]) return -9;
    for (int fd = 3; fd < MAX_FDS; fd++) {
        if (!fd_table[fd]) {
            fd_table[fd] = fd_table[oldfd];
            fd_offset[fd] = 0;
            return fd;
        }
    }
    return -24;
}

/* dup2: дублирует в указанный slot, закрывая старое содержимое */
int vfs_dup2(int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= MAX_FDS || !fd_table[oldfd]) return -9;
    if (newfd < 0 || newfd >= MAX_FDS) return -9;
    if (oldfd == newfd) return newfd;
    /* close newfd если открыт (мы не уменьшаем pipe refs пока — заметка для будущего) */
    fd_table[newfd] = fd_table[oldfd];
    fd_offset[newfd] = 0;
    return newfd;
}

/* Освободить vnode, если его FS поддерживает release (динамические vnode). */
void vnode_release(vnode_t* v) {
    if (v && v->ops && v->ops->release) v->ops->release(v);
}

/* Размер файла по fd (для fstat). */
off_t vfs_fsize(int fd) {
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd]) return -1;
    return (off_t)fd_table[fd]->size;
}

/* Тип файла по fd (1=file, 2=dir). */
int vfs_ftype(int fd) {
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd]) return -1;
    return fd_table[fd]->type;
}

/* helpers для getdents64 */
static long dir_pos_table[MAX_FDS];  /* MAX_FDS = 64 обычно, с запасом */

vnode_t* vfs_vnode_of_fd(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return fd_table[fd];
}
long vfs_get_dir_pos(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return 0;
    return dir_pos_table[fd];
}
void vfs_set_dir_pos(int fd, long pos) {
    if (fd < 0 || fd >= MAX_FDS) return;
    dir_pos_table[fd] = pos;
}
