/*
 * ramfs.c — простая writable in-memory FS поверх VFS.
 *
 * Дерево узлов в kernel heap'е. Файлы — динамический buffer,
 * растёт по мере write. Директории — список детей.
 *
 * Цель: дать /tmp в котором gcc сможет создавать temp files.
 * В будущем заменится на ext2 поверх virtio-blk.
 */

#include "types.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "vfs.h"
#include "ramfs.h"

extern void* kmalloc(size_t);
extern void  kfree(void*);

typedef struct ramfs_node {
    char name[64];
    int  type;           /* VNODE_FILE / VNODE_DIR */
    /* File payload */
    char* data;
    size_t size;
    size_t capacity;
    /* Dir children */
    struct ramfs_node* first_child;
    struct ramfs_node* sibling;
    vnode_t vnode;
    vnode_t* mount;      /* если != NULL — это mountpoint, перенаправляем сюда */
} ramfs_node_t;

/* ---------- VFS ops ---------- */

static ssize_t rfs_read(vnode_t* v, void* buf, size_t n, off_t off) {
    ramfs_node_t* node = (ramfs_node_t*)v->priv;
    if ((u64)off >= node->size) return 0;
    size_t avail = node->size - (size_t)off;
    size_t cnt = (n < avail) ? n : avail;
    memcpy(buf, node->data + off, cnt);
    return (ssize_t)cnt;
}

static ssize_t rfs_write(vnode_t* v, const void* buf, size_t n, off_t off) {
    ramfs_node_t* node = (ramfs_node_t*)v->priv;
    size_t end = (size_t)off + n;

    /* Расширим capacity при необходимости */
    if (end > node->capacity) {
        size_t new_cap = node->capacity ? node->capacity * 2 : 64;
        while (new_cap < end) new_cap *= 2;
        char* new_data = (char*)kmalloc(new_cap);
        if (!new_data) return -12;     /* -ENOMEM */
        if (node->data) {
            memcpy(new_data, node->data, node->size);
            kfree(node->data);
        }
        node->data = new_data;
        node->capacity = new_cap;
    }
    if (end > node->size) {
        /* "Растягиваем" — обнуляем gap если есть */
        if ((size_t)off > node->size) {
            memset(node->data + node->size, 0, (size_t)off - node->size);
        }
        node->size = end;
        v->size = end;
    }
    memcpy(node->data + off, buf, n);
    return (ssize_t)n;
}

static vnode_t* rfs_lookup(vnode_t* dir, const char* name) {
    ramfs_node_t* d = (ramfs_node_t*)dir->priv;
    for (ramfs_node_t* c = d->first_child; c; c = c->sibling) {
        if (!strcmp(c->name, name)) {
            /* Если ребёнок — mountpoint, возвращаем смонтированный корень */
            if (c->mount) return c->mount;
            return &c->vnode;
        }
    }
    return NULL;
}

static int rfs_readdir(vnode_t* dir, size_t idx, char* name_out, size_t name_max) {
    ramfs_node_t* d = (ramfs_node_t*)dir->priv;
    size_t i = 0;
    for (ramfs_node_t* c = d->first_child; c; c = c->sibling, i++) {
        if (i == idx) {
            size_t len = strlen(c->name);
            if (len >= name_max) len = name_max - 1;
            memcpy(name_out, c->name, len);
            name_out[len] = '\0';
            return (int)len;
        }
    }
    return -1;
}

static const struct vnode_ops rfs_file_ops = {
    .read = rfs_read, .write = rfs_write, .lookup = NULL, .readdir = NULL,
};
static const struct vnode_ops rfs_dir_ops = {
    .read = NULL, .write = NULL, .lookup = rfs_lookup, .readdir = rfs_readdir,
};

/* ---------- Создание узлов ---------- */

static ramfs_node_t* node_new(const char* name, int type) {
    ramfs_node_t* n = (ramfs_node_t*)kmalloc(sizeof(*n));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    size_t l = strlen(name);
    if (l >= sizeof(n->name)) l = sizeof(n->name) - 1;
    memcpy(n->name, name, l);
    n->name[l] = '\0';
    n->type = type;
    n->vnode.type = type;
    n->vnode.ops = (type == VNODE_DIR) ? &rfs_dir_ops : &rfs_file_ops;
    n->vnode.priv = n;
    return n;
}

static ramfs_node_t* g_root = NULL;

vnode_t* ramfs_init(void) {
    g_root = node_new("/", VNODE_DIR);
    return &g_root->vnode;
}

/* Создаём новый файл в директории. Если уже есть — возвращаем существующий. */
vnode_t* ramfs_create_file(vnode_t* dir, const char* name) {
    ramfs_node_t* d = (ramfs_node_t*)dir->priv;
    /* Проверим что нет уже */
    for (ramfs_node_t* c = d->first_child; c; c = c->sibling) {
        if (!strcmp(c->name, name)) return &c->vnode;
    }
    ramfs_node_t* n = node_new(name, VNODE_FILE);
    if (!n) return NULL;
    n->sibling = d->first_child;
    d->first_child = n;
    return &n->vnode;
}

vnode_t* ramfs_create_dir(vnode_t* parent, const char* name) {
    ramfs_node_t* d = (ramfs_node_t*)parent->priv;
    for (ramfs_node_t* c = d->first_child; c; c = c->sibling) {
        if (!strcmp(c->name, name)) return &c->vnode;
    }
    ramfs_node_t* n = node_new(name, VNODE_DIR);
    if (!n) return NULL;
    n->sibling = d->first_child;
    d->first_child = n;
    return &n->vnode;
}

/* Удалить файл по имени в директории. Возвращает 0 если ок. */
int ramfs_unlink(vnode_t* dir, const char* name) {
    ramfs_node_t* d = (ramfs_node_t*)dir->priv;
    ramfs_node_t** pp = &d->first_child;
    while (*pp) {
        if (!strcmp((*pp)->name, name)) {
            ramfs_node_t* victim = *pp;
            *pp = victim->sibling;
            if (victim->data) kfree(victim->data);
            kfree(victim);
            return 0;
        }
        pp = &(*pp)->sibling;
    }
    return -2;   /* -ENOENT */
}

vnode_t* ramfs_root(void) {
    return g_root ? &g_root->vnode : NULL;
}

/* Примонтировать чужой vnode (например ext2 root) как поддиректорию.
   lookup на этом имени вернёт mounted_root вместо ramfs-узла. */
vnode_t* ramfs_mount_at(vnode_t* parent, const char* name, vnode_t* mounted_root) {
    ramfs_node_t* d = (ramfs_node_t*)parent->priv;
    /* Если узел уже есть — переиспользуем, иначе создаём dir-узел */
    ramfs_node_t* n = NULL;
    for (ramfs_node_t* c = d->first_child; c; c = c->sibling) {
        if (!strcmp(c->name, name)) { n = c; break; }
    }
    if (!n) {
        n = node_new(name, VNODE_DIR);
        if (!n) return NULL;
        n->sibling = d->first_child;
        d->first_child = n;
    }
    n->mount = mounted_root;
    return mounted_root;
}
