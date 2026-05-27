/*
 * tarfs.c — read-only FS поверх tar-архива в памяти (initrd).
 *
 * Формат USTAR: каждый файл предваряется 512-байтным заголовком.
 *   - name      (100 байт, NUL-terminated если короче)
 *   - mode      (8 байт octal ASCII)
 *   - uid/gid   (8 байт каждый)
 *   - size      (12 байт octal ASCII)
 *   - mtime     (12 байт)
 *   - checksum  (8 байт)
 *   - typeflag  (1 байт: '0'/'\0' = regular, '5' = directory)
 *   - linkname  (100)
 *   - magic     "ustar\0" + version "00"
 *   - uname/gname (32 each)
 *   - ...
 * После заголовка идёт payload, выровненный до 512 байт.
 * Конец архива — два подряд NUL-блока.
 *
 * Наш tarfs строит in-memory дерево vnode'ов один раз при init,
 * чтобы lookup был O(имена_в_пути). Это много памяти на большой
 * initrd, но для bring-up'а норм.
 */

#include "types.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "vfs.h"
#include "tarfs.h"

typedef struct PACKED {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} tar_header_t;

/* ---------- Структуры in-memory ---------- */

typedef struct tar_node {
    char name[64];
    int  type;                    /* VNODE_FILE | VNODE_DIR */
    const u8* data;               /* для файлов */
    u64  size;
    struct tar_node* first_child; /* для директорий */
    struct tar_node* sibling;
    vnode_t vnode;                /* публичный vnode */
} tar_node_t;

/* ---------- octal-string → u64 ---------- */
static u64 parse_octal(const char* s, size_t n) {
    u64 v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) {
        v = (v << 3) | (u64)(s[i] - '0');
    }
    return v;
}

/* ---------- VFS ops ---------- */

static ssize_t tar_read(vnode_t* v, void* buf, size_t n, off_t off) {
    tar_node_t* node = (tar_node_t*)v->priv;
    if ((u64)off >= node->size) return 0;
    u64 avail = node->size - (u64)off;
    size_t cnt = (n < avail) ? n : (size_t)avail;
    memcpy(buf, node->data + off, cnt);
    return (ssize_t)cnt;
}

static vnode_t* tar_lookup(vnode_t* dir, const char* name) {
    tar_node_t* d = (tar_node_t*)dir->priv;
    for (tar_node_t* c = d->first_child; c; c = c->sibling) {
        if (!strcmp(c->name, name)) return &c->vnode;
    }
    return NULL;
}

static int tar_readdir(vnode_t* dir, size_t idx, char* name_out, size_t name_max) {
    tar_node_t* d = (tar_node_t*)dir->priv;
    size_t i = 0;
    for (tar_node_t* c = d->first_child; c; c = c->sibling, i++) {
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

static const struct vnode_ops tar_file_ops = {
    .read = tar_read, .write = NULL, .lookup = NULL, .readdir = NULL,
};
static const struct vnode_ops tar_dir_ops = {
    .read = NULL, .write = NULL, .lookup = tar_lookup, .readdir = tar_readdir,
};

/* ---------- Построение дерева ---------- */

static tar_node_t* root_node = NULL;

/*
 * Извлечь "последнюю компоненту" из tar-имени (которое может
 * содержать '/'). Возвращает указатель на имя и заполняет parent_path.
 * Простая реализация: ищем последний '/'.
 */
static const char* last_component(const char* path, char* parent_buf, size_t parent_buf_n) {
    const char* slash = NULL;
    for (const char* p = path; *p; p++) if (*p == '/') slash = p;
    if (!slash) {
        if (parent_buf_n) parent_buf[0] = '\0';
        return path;
    }
    size_t plen = (size_t)(slash - path);
    if (plen >= parent_buf_n) plen = parent_buf_n - 1;
    memcpy(parent_buf, path, plen);
    parent_buf[plen] = '\0';
    return slash + 1;
}

/* Найти/создать узел директории по пути (поддерживает создание
   intermediate директорий, если их не было в архиве). */
static tar_node_t* find_or_make_dir(const char* path) {
    if (!*path) return root_node;

    tar_node_t* cur = root_node;
    const char* p = path;
    char comp[64];

    while (*p) {
        size_t i = 0;
        while (*p && *p != '/' && i + 1 < sizeof(comp)) comp[i++] = *p++;
        comp[i] = '\0';
        if (*p == '/') p++;
        if (i == 0) continue;

        /* Ищем среди детей */
        tar_node_t* child = NULL;
        for (tar_node_t* c = cur->first_child; c; c = c->sibling) {
            if (!strcmp(c->name, comp)) { child = c; break; }
        }
        if (!child) {
            child = (tar_node_t*)malloc(sizeof(*child));
            if (!child) return NULL;
            memset(child, 0, sizeof(*child));
            size_t l = strlen(comp);
            if (l >= sizeof(child->name)) l = sizeof(child->name) - 1;
            memcpy(child->name, comp, l);
            child->name[l] = '\0';
            child->type = VNODE_DIR;
            child->vnode.type = VNODE_DIR;
            child->vnode.ops  = &tar_dir_ops;
            child->vnode.priv = child;
            child->sibling = cur->first_child;
            cur->first_child = child;
        }
        cur = child;
    }
    return cur;
}

vnode_t* tarfs_init(const void* archive, size_t size) {
    /* root */
    root_node = (tar_node_t*)malloc(sizeof(*root_node));
    if (!root_node) return NULL;
    memset(root_node, 0, sizeof(*root_node));
    strcpy(root_node->name, "/");
    root_node->type = VNODE_DIR;
    root_node->vnode.type = VNODE_DIR;
    root_node->vnode.ops  = &tar_dir_ops;
    root_node->vnode.priv = root_node;

    const u8* p = (const u8*)archive;
    const u8* end = p + size;

    int file_count = 0;
    while (p + 512 <= end) {
        const tar_header_t* h = (const tar_header_t*)p;
        if (h->name[0] == '\0') break;    /* end of archive */

        /* Проверим magic — допускаем USTAR и старый формат */
        u64 fsize = parse_octal(h->size, sizeof(h->size));

        /* Нормализуем имя — убираем trailing '/' для директорий */
        char name[100];
        size_t nl = 0;
        while (nl < sizeof(h->name) && h->name[nl] && nl < sizeof(name)-1) {
            name[nl] = h->name[nl];
            nl++;
        }
        name[nl] = '\0';
        if (nl > 0 && name[nl-1] == '/') name[--nl] = '\0';

        /* Распознаём тип */
        int is_dir = (h->typeflag == '5') || (nl > 0 && h->name[nl] == '/');
        if (h->typeflag == 'L' || h->typeflag == 'x' || h->typeflag == 'g') {
            /* GNU long name / pax — пропустим */
            p += 512;
            p += (fsize + 511) & ~511ULL;
            continue;
        }

        /* Найдём parent dir */
        char parent[100];
        const char* leaf = last_component(name, parent, sizeof(parent));
        tar_node_t* parent_dir = find_or_make_dir(parent);
        if (!parent_dir) break;

        if (is_dir) {
            /* Создадим директорию через find_or_make_dir */
            find_or_make_dir(name);
        } else {
            /* Регулярный файл */
            tar_node_t* node = (tar_node_t*)malloc(sizeof(*node));
            if (!node) break;
            memset(node, 0, sizeof(*node));
            size_t ll = strlen(leaf);
            if (ll >= sizeof(node->name)) ll = sizeof(node->name) - 1;
            memcpy(node->name, leaf, ll);
            node->name[ll] = '\0';
            node->type = VNODE_FILE;
            node->size = fsize;
            node->data = p + 512;
            node->vnode.type = VNODE_FILE;
            node->vnode.size = fsize;
            node->vnode.ops  = &tar_file_ops;
            node->vnode.priv = node;
            node->sibling = parent_dir->first_child;
            parent_dir->first_child = node;
            file_count++;
        }

        /* Сдвигаемся на header + размер данных (выровн. до 512) */
        p += 512;
        p += (fsize + 511) & ~511ULL;
    }

    printf("[tarfs] parsed initrd: %d files\n", file_count);
    return &root_node->vnode;
}

/* Прицепляем устройство как файл в корень */
void tarfs_add_device(const char* name, vnode_t* dev) {
    tar_node_t* node = (tar_node_t*)malloc(sizeof(*node));
    if (!node) return;
    memset(node, 0, sizeof(*node));
    size_t l = strlen(name);
    if (l >= sizeof(node->name)) l = sizeof(node->name) - 1;
    memcpy(node->name, name, l);
    node->name[l] = '\0';
    /* "оборачиваем" чужой vnode: создаём отдельный, который делегирует */
    node->vnode = *dev;
    node->sibling = root_node->first_child;
    root_node->first_child = node;
}
