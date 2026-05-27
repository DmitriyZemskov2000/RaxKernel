/*
 * ext2.c — read-only драйвер ext2 поверх virtio-blk.
 *
 * Поддерживает: чтение superblock, block group descriptors, inode'ов,
 * директорий и файлов (direct + single/double indirect blocks).
 *
 * Block size берётся из superblock (у нас 1024). Сектор = 512,
 * поэтому блок = (block_size/512) секторов.
 *
 * Интеграция с VFS: ext2_mount возвращает корневой vnode, который
 * подключается в дерево VFS. lookup/read/readdir реализованы поверх
 * ext2-структур.
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include "virtio_blk.h"
#include "vfs.h"

extern void* kmalloc(size_t);
extern void  kfree(void*);

/* ---- on-disk структуры ---- */

typedef struct {
    u32 inodes_count;
    u32 blocks_count;
    u32 r_blocks_count;
    u32 free_blocks_count;
    u32 free_inodes_count;
    u32 first_data_block;
    u32 log_block_size;       /* block size = 1024 << this */
    u32 log_frag_size;
    u32 blocks_per_group;
    u32 frags_per_group;
    u32 inodes_per_group;
    u32 mtime;
    u32 wtime;
    u16 mnt_count;
    u16 max_mnt_count;
    u16 magic;                /* 0xEF53 */
    u16 state;
    u16 errors;
    u16 minor_rev;
    u32 lastcheck;
    u32 checkinterval;
    u32 creator_os;
    u32 rev_level;
    u16 def_resuid;
    u16 def_resgid;
    /* EXT2_DYNAMIC_REV поля */
    u32 first_ino;
    u16 inode_size;
    u16 block_group_nr;
    u32 feature_compat;
    u32 feature_incompat;
    u32 feature_ro_compat;
    u8  uuid[16];
    char volume_name[16];
} __attribute__((packed)) ext2_superblock_t;

typedef struct {
    u32 block_bitmap;
    u32 inode_bitmap;
    u32 inode_table;
    u16 free_blocks_count;
    u16 free_inodes_count;
    u16 used_dirs_count;
    u16 pad;
    u8  reserved[12];
} __attribute__((packed)) ext2_group_desc_t;

typedef struct {
    u16 mode;
    u16 uid;
    u32 size;
    u32 atime;
    u32 ctime;
    u32 mtime;
    u32 dtime;
    u16 gid;
    u16 links_count;
    u32 blocks;       /* в единицах 512-байтовых секторов */
    u32 flags;
    u32 osd1;
    u32 block[15];    /* 0-11 direct, 12 single, 13 double, 14 triple */
    u32 generation;
    u32 file_acl;
    u32 dir_acl;
    u32 faddr;
    u8  osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    u32 inode;
    u16 rec_len;
    u8  name_len;
    u8  file_type;
    char name[];      /* name_len байт */
} __attribute__((packed)) ext2_dir_entry_t;

#define EXT2_MAGIC      0xEF53
#define EXT2_ROOT_INO   2
#define EXT2_S_IFDIR    0x4000
#define EXT2_S_IFREG    0x8000

/* ---- состояние тома ---- */
static ext2_superblock_t sb;
static u32 block_size;
static u32 sectors_per_block;
static u32 num_groups;
static ext2_group_desc_t* group_desc;   /* массив дескрипторов групп */
static int ext2_ready = 0;

/* ---- низкоуровневый block I/O через virtio ---- */

/* Bounce buffer в .bss (физически = виртуально, identity-mapped в нижней
   памяти) — virtio DMA требует физических адресов, а kmalloc отдаёт
   higher-half heap. Читаем в bounce, потом копируем в целевой буфер. */
static u8 ext2_bounce[4096] __attribute__((aligned(512)));
static u8 ext2_wbounce[4096] __attribute__((aligned(512)));

static int read_block(u32 block, void* buf) {
    u64 sector = (u64)block * sectors_per_block;
    for (u32 i = 0; i < sectors_per_block; i++) {
        if (virtio_blk_read(sector + i, ext2_bounce + i * 512) != 0) return -1;
    }
    memcpy(buf, ext2_bounce, block_size);
    return 0;
}

static int write_block(u32 block, const void* buf) {
    u64 sector = (u64)block * sectors_per_block;
    memcpy(ext2_wbounce, buf, block_size);
    for (u32 i = 0; i < sectors_per_block; i++) {
        if (virtio_blk_write(sector + i, ext2_wbounce + i * 512) != 0) return -1;
    }
    return 0;
}

/* Чтение inode по номеру (1-based) */
static int read_inode(u32 ino, ext2_inode_t* out) {
    if (ino == 0) return -1;
    u32 group = (ino - 1) / sb.inodes_per_group;
    u32 index = (ino - 1) % sb.inodes_per_group;
    if (group >= num_groups) return -1;

    u32 inode_table_block = group_desc[group].inode_table;
    u32 inode_size = sb.inode_size ? sb.inode_size : 128;
    u32 offset = index * inode_size;
    u32 block = inode_table_block + offset / block_size;
    u32 off_in_block = offset % block_size;

    u8* blockbuf = (u8*)kmalloc(block_size);
    if (!blockbuf) return -1;
    if (read_block(block, blockbuf) != 0) { kfree(blockbuf); return -1; }
    memcpy(out, blockbuf + off_in_block, sizeof(ext2_inode_t));
    kfree(blockbuf);
    return 0;
}

/* Запись inode на диск (read-modify-write блока inode table) */
static int write_inode(u32 ino, const ext2_inode_t* in) {
    if (ino == 0) return -1;
    u32 group = (ino - 1) / sb.inodes_per_group;
    u32 index = (ino - 1) % sb.inodes_per_group;
    if (group >= num_groups) return -1;

    u32 inode_table_block = group_desc[group].inode_table;
    u32 inode_size = sb.inode_size ? sb.inode_size : 128;
    u32 offset = index * inode_size;
    u32 block = inode_table_block + offset / block_size;
    u32 off_in_block = offset % block_size;

    u8* blockbuf = (u8*)kmalloc(block_size);
    if (!blockbuf) return -1;
    if (read_block(block, blockbuf) != 0) { kfree(blockbuf); return -1; }
    memcpy(blockbuf + off_in_block, in, sizeof(ext2_inode_t));
    int r = write_block(block, blockbuf);
    kfree(blockbuf);
    return r;
}

/* Сохранить superblock (block 1) и BGDT (block 2) на диск */
static int sync_metadata(void) {
    u8* buf = (u8*)kmalloc(block_size);
    if (!buf) return -1;
    /* superblock: читаем блок, патчим первые sizeof(sb), пишем */
    if (read_block(1, buf) != 0) { kfree(buf); return -1; }
    memcpy(buf, &sb, sizeof(sb));
    write_block(1, buf);
    kfree(buf);

    /* BGDT на block 2 (block_size==1024) */
    u32 bgdt_block = (block_size == 1024) ? 2 : 1;
    u32 bgdt_size = num_groups * sizeof(ext2_group_desc_t);
    u32 bgdt_blocks = (bgdt_size + block_size - 1) / block_size;
    for (u32 i = 0; i < bgdt_blocks; i++) {
        write_block(bgdt_block + i, (u8*)group_desc + i * block_size);
    }
    return 0;
}

/* Выделить свободный блок данных. Возвращает block номер или 0. */
static u32 alloc_block(void) {
    for (u32 g = 0; g < num_groups; g++) {
        if (group_desc[g].free_blocks_count == 0) continue;
        u8* bmp = (u8*)kmalloc(block_size);
        if (!bmp) return 0;
        if (read_block(group_desc[g].block_bitmap, bmp) != 0) { kfree(bmp); return 0; }
        for (u32 i = 0; i < sb.blocks_per_group; i++) {
            if (!(bmp[i/8] & (1 << (i%8)))) {
                bmp[i/8] |= (1 << (i%8));
                write_block(group_desc[g].block_bitmap, bmp);
                kfree(bmp);
                group_desc[g].free_blocks_count--;
                sb.free_blocks_count--;
                sync_metadata();
                /* блок номер: first_data_block + g*blocks_per_group + i */
                return sb.first_data_block + g * sb.blocks_per_group + i;
            }
        }
        kfree(bmp);
    }
    printf("[ext2] alloc_block: exhausted (sb.free=%u)\n", sb.free_blocks_count);
    return 0;
}

/* Выделить свободный inode. Возвращает inode номер (1-based) или 0. */
static u32 alloc_inode(void) {
    for (u32 g = 0; g < num_groups; g++) {
        if (group_desc[g].free_inodes_count == 0) continue;
        u8* bmp = (u8*)kmalloc(block_size);
        if (!bmp) return 0;
        if (read_block(group_desc[g].inode_bitmap, bmp) != 0) { kfree(bmp); return 0; }
        for (u32 i = 0; i < sb.inodes_per_group; i++) {
            if (!(bmp[i/8] & (1 << (i%8)))) {
                bmp[i/8] |= (1 << (i%8));
                write_block(group_desc[g].inode_bitmap, bmp);
                kfree(bmp);
                group_desc[g].free_inodes_count--;
                sb.free_inodes_count--;
                sync_metadata();
                return g * sb.inodes_per_group + i + 1;   /* 1-based */
            }
        }
        kfree(bmp);
    }
    return 0;
}

/*
 * Чтение n-го логического блока файла (с учётом indirect).
 * Возвращает физический block номер или 0 если дыра.
 */
static u32 get_file_block(ext2_inode_t* inode, u32 logical) {
    u32 ptrs_per_block = block_size / 4;

    if (logical < 12) {
        return inode->block[logical];
    }
    logical -= 12;

    /* single indirect */
    if (logical < ptrs_per_block) {
        if (!inode->block[12]) return 0;
        u32* ind = (u32*)kmalloc(block_size);
        if (!ind) return 0;
        if (read_block(inode->block[12], ind) != 0) { kfree(ind); return 0; }
        u32 r = ind[logical];
        kfree(ind);
        return r;
    }
    logical -= ptrs_per_block;

    /* double indirect */
    if (logical < ptrs_per_block * ptrs_per_block) {
        if (!inode->block[13]) return 0;
        u32* d1 = (u32*)kmalloc(block_size);
        if (!d1) return 0;
        if (read_block(inode->block[13], d1) != 0) { kfree(d1); return 0; }
        u32 idx1 = logical / ptrs_per_block;
        u32 idx2 = logical % ptrs_per_block;
        u32 b1 = d1[idx1];
        kfree(d1);
        if (!b1) return 0;
        u32* d2 = (u32*)kmalloc(block_size);
        if (!d2) return 0;
        if (read_block(b1, d2) != 0) { kfree(d2); return 0; }
        u32 r = d2[idx2];
        kfree(d2);
        return r;
    }

    /* triple indirect — для наших файлов не нужно */
    return 0;
}

/* Чтение данных файла в buf, начиная с offset, размером n байт */
static int ext2_read_file(ext2_inode_t* inode, void* buf, u32 offset, u32 n) {
    u32 size = inode->size;
    if (offset >= size) return 0;
    if (offset + n > size) n = size - offset;

    u8* out = (u8*)buf;
    u32 done = 0;
    u8* blockbuf = (u8*)kmalloc(block_size);
    if (!blockbuf) return -1;

    while (done < n) {
        u32 file_off = offset + done;
        u32 logical = file_off / block_size;
        u32 boff = file_off % block_size;
        u32 chunk = block_size - boff;
        if (chunk > n - done) chunk = n - done;

        u32 phys = get_file_block(inode, logical);
        if (phys == 0) {
            /* дыра — нули */
            memset(out + done, 0, chunk);
        } else {
            if (read_block(phys, blockbuf) != 0) { kfree(blockbuf); return -1; }
            memcpy(out + done, blockbuf + boff, chunk);
        }
        done += chunk;
    }
    kfree(blockbuf);
    return (int)done;
}

/*
 * Установить физ. блок для logical-индекса файла, выделяя при
 * необходимости (direct + single indirect). inode_dirty — caller
 * запишет inode позже. Возвращает физ. блок или 0 при ошибке.
 */
static u32 set_file_block(ext2_inode_t* inode, u32 logical) {
    u32 ptrs_per_block = block_size / 4;

    if (logical < 12) {
        if (!inode->block[logical]) {
            u32 b = alloc_block();
            if (!b) return 0;
            inode->block[logical] = b;
            inode->blocks += block_size / 512;
        }
        return inode->block[logical];
    }
    logical -= 12;

    /* single indirect */
    if (logical < ptrs_per_block) {
        if (!inode->block[12]) {
            u32 ib = alloc_block();
            if (!ib) return 0;
            inode->block[12] = ib;
            inode->blocks += block_size / 512;
            /* обнулим indirect блок */
            u8* z = (u8*)kmalloc(block_size);
            if (z) { memset(z, 0, block_size); write_block(ib, z); kfree(z); }
        }
        u32* ind = (u32*)kmalloc(block_size);
        if (!ind) return 0;
        if (read_block(inode->block[12], ind) != 0) { kfree(ind); return 0; }
        if (!ind[logical]) {
            u32 b = alloc_block();
            if (!b) { kfree(ind); return 0; }
            ind[logical] = b;
            inode->blocks += block_size / 512;
            write_block(inode->block[12], ind);
        }
        u32 r = ind[logical];
        kfree(ind);
        return r;
    }
    /* double indirect — не поддержано для записи */
    return 0;
}

/* Запись n байт в файл с offset. Выделяет блоки. Обновляет inode->size. */
static int ext2_write_file(ext2_inode_t* inode, const void* buf, u32 offset, u32 n) {
    const u8* in = (const u8*)buf;
    u32 done = 0;
    u8* blockbuf = (u8*)kmalloc(block_size);
    if (!blockbuf) return -1;

    while (done < n) {
        u32 file_off = offset + done;
        u32 logical = file_off / block_size;
        u32 boff = file_off % block_size;
        u32 chunk = block_size - boff;
        if (chunk > n - done) chunk = n - done;

        u32 phys = set_file_block(inode, logical);
        if (phys == 0) {
            printf("[ext2] write: set_file_block(lb=%u) FAILED (size so far %u)\n", logical, done);
            kfree(blockbuf); return -1;
        }

        /* read-modify-write если частичный блок */
        if (boff != 0 || chunk != block_size) {
            if (read_block(phys, blockbuf) != 0) memset(blockbuf, 0, block_size);
        }
        memcpy(blockbuf + boff, in + done, chunk);
        if (write_block(phys, blockbuf) != 0) { kfree(blockbuf); return -1; }
        done += chunk;
    }
    kfree(blockbuf);

    if (offset + n > inode->size) inode->size = offset + n;
    return (int)done;
}

/* ---- VFS интеграция ---- */

/* priv каждого vnode хранит inode-номер. */
typedef struct {
    u32 ino;
    ext2_inode_t inode;
} ext2_vnode_priv_t;

static vnode_t* ext2_make_vnode(u32 ino);

static ssize_t ext2_vfs_read(vnode_t* v, void* buf, size_t n, off_t off) {
    ext2_vnode_priv_t* pv = (ext2_vnode_priv_t*)v->priv;
    int r = ext2_read_file(&pv->inode, buf, (u32)off, (u32)n);
    return (r < 0) ? -1 : r;
}

static ssize_t ext2_vfs_write(vnode_t* v, const void* buf, size_t n, off_t off) {
    ext2_vnode_priv_t* pv = (ext2_vnode_priv_t*)v->priv;
    int r = ext2_write_file(&pv->inode, buf, (u32)off, (u32)n);
    if (r < 0) return -1;
    /* Сохраняем обновлённый inode (size, block указатели) на диск */
    write_inode(pv->ino, &pv->inode);
    v->size = pv->inode.size;
    return r;
}

static vnode_t* ext2_vfs_lookup(vnode_t* v, const char* name) {
    ext2_vnode_priv_t* pv = (ext2_vnode_priv_t*)v->priv;
    if (!(pv->inode.mode & EXT2_S_IFDIR)) return NULL;

    u8* dirbuf = (u8*)kmalloc(pv->inode.size);
    if (!dirbuf) return NULL;
    int sz = ext2_read_file(&pv->inode, dirbuf, 0, pv->inode.size);
    if (sz <= 0) { kfree(dirbuf); return NULL; }

    vnode_t* result = NULL;
    u32 pos = 0;
    int seen = 0;
    while (pos < (u32)sz) {
        ext2_dir_entry_t* de = (ext2_dir_entry_t*)(dirbuf + pos);
        if (de->rec_len == 0) break;
        if (de->inode != 0 && de->name_len == strlen(name) &&
            memcmp(de->name, name, de->name_len) == 0) {
            result = ext2_make_vnode(de->inode);
            break;
        }
        seen++;
        pos += de->rec_len;
    }
    if (!result) { /* тихо — нормально для multi-path include */ }
    kfree(dirbuf);
    return result;
}

static int ext2_vfs_readdir(vnode_t* v, size_t index, char* name_out, size_t name_cap) {
    ext2_vnode_priv_t* pv = (ext2_vnode_priv_t*)v->priv;
    if (!(pv->inode.mode & EXT2_S_IFDIR)) return -1;

    u8* dirbuf = (u8*)kmalloc(pv->inode.size);
    if (!dirbuf) return -1;
    int sz = ext2_read_file(&pv->inode, dirbuf, 0, pv->inode.size);
    if (sz <= 0) { kfree(dirbuf); return -1; }

    u32 pos = 0;
    size_t cur = 0;
    int ret = -1;
    while (pos < (u32)sz) {
        ext2_dir_entry_t* de = (ext2_dir_entry_t*)(dirbuf + pos);
        if (de->rec_len == 0) break;
        if (de->inode != 0) {
            if (cur == index) {
                size_t l = de->name_len;
                if (l >= name_cap) l = name_cap - 1;
                memcpy(name_out, de->name, l);
                name_out[l] = '\0';
                ret = (int)l;
                break;
            }
            cur++;
        }
        pos += de->rec_len;
    }
    kfree(dirbuf);
    return ret;
}

/* Добавить directory entry в директорию. */
static int ext2_add_dirent(ext2_vnode_priv_t* dir, const char* name, u32 ino, u8 ftype) {
    u32 nlen = strlen(name);
    u32 need = 8 + ((nlen + 3) & ~3u);

    u8* blockbuf = (u8*)kmalloc(block_size);
    if (!blockbuf) return -1;

    u32 nblocks = (dir->inode.size + block_size - 1) / block_size;
    for (u32 lb = 0; lb < nblocks; lb++) {
        u32 phys = get_file_block(&dir->inode, lb);
        if (!phys) continue;
        if (read_block(phys, blockbuf) != 0) continue;

        u32 pos = 0;
        while (pos < block_size) {
            ext2_dir_entry_t* de = (ext2_dir_entry_t*)(blockbuf + pos);
            if (de->rec_len == 0) break;
            u32 actual = de->inode ? (8 + ((de->name_len + 3) & ~3u)) : 0;
            u32 slack = de->rec_len - actual;
            if (slack >= need) {
                u32 newpos = pos + actual;
                u32 newrec = de->rec_len - actual;
                if (de->inode) de->rec_len = actual;
                ext2_dir_entry_t* ne = (ext2_dir_entry_t*)(blockbuf + newpos);
                ne->inode = ino;
                ne->rec_len = newrec;
                ne->name_len = nlen;
                ne->file_type = ftype;
                memcpy(ne->name, name, nlen);
                write_block(phys, blockbuf);
                kfree(blockbuf);
                return 0;
            }
            pos += de->rec_len;
        }
    }

    /* Места в существующих блоках нет — добавляем новый блок к директории. */
    {
        u32 next_lb = nblocks;
        /* set_file_block выделит новый блок под следующий логический и
           вернёт его физический номер. */
        u32 newblk = set_file_block(&dir->inode, next_lb);
        if (!newblk) { kfree(blockbuf); return -1; }
        printf("[ext2] dir-grow: added block lb=%u phys=%u for '%s', newsize=%u\n",
               next_lb, newblk, name, dir->inode.size + block_size);

        memset(blockbuf, 0, block_size);
        ext2_dir_entry_t* ne = (ext2_dir_entry_t*)blockbuf;
        ne->inode = ino;
        ne->rec_len = block_size;          /* занимает весь блок */
        ne->name_len = nlen;
        ne->file_type = ftype;
        memcpy(ne->name, name, nlen);
        if (write_block(newblk, blockbuf) != 0) { kfree(blockbuf); return -1; }

        dir->inode.size += block_size;
        write_inode(dir->ino, &dir->inode);
        kfree(blockbuf);
        return 0;
    }
}

static vnode_t* ext2_vfs_create(vnode_t* dirv, const char* name) {
    ext2_vnode_priv_t* dir = (ext2_vnode_priv_t*)dirv->priv;
    if (!(dir->inode.mode & EXT2_S_IFDIR)) return NULL;

    u32 ino = alloc_inode();
    if (!ino) { printf("[ext2] create %s: alloc_inode FAILED\n", name); return NULL; }

    ext2_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    ni.mode = EXT2_S_IFREG | 0644;
    ni.links_count = 1;
    ni.size = 0;
    write_inode(ino, &ni);

    if (ext2_add_dirent(dir, name, ino, 1) != 0) {
        printf("[ext2] create %s: add_dirent FAILED (ino=%u)\n", name, ino);
        return NULL;
    }

    return ext2_make_vnode(ino);
}

static vnode_t* ext2_root_vnode = NULL;   /* постоянный — не освобождать */

static void ext2_vfs_release(vnode_t* v) {
    if (!v) return;
    if (v == ext2_root_vnode) return;   /* mount root живёт всегда */
    if (v->priv) kfree(v->priv);
    kfree(v);
}

static const struct vnode_ops ext2_dir_ops = {
    .read = NULL,
    .write = NULL,
    .lookup = ext2_vfs_lookup,
    .readdir = ext2_vfs_readdir,
    .create = ext2_vfs_create,
    .release = ext2_vfs_release,
};

static const struct vnode_ops ext2_file_ops = {
    .read = ext2_vfs_read,
    .write = ext2_vfs_write,
    .lookup = NULL,
    .readdir = NULL,
    .create = NULL,
    .release = ext2_vfs_release,
};

static vnode_t* ext2_make_vnode(u32 ino) {
    ext2_vnode_priv_t* pv = (ext2_vnode_priv_t*)kmalloc(sizeof(ext2_vnode_priv_t));
    if (!pv) return NULL;
    pv->ino = ino;
    if (read_inode(ino, &pv->inode) != 0) { kfree(pv); return NULL; }

    vnode_t* v = (vnode_t*)kmalloc(sizeof(vnode_t));
    if (!v) { kfree(pv); return NULL; }
    memset(v, 0, sizeof(*v));

    if (pv->inode.mode & EXT2_S_IFDIR) {
        v->type = VNODE_DIR;
        v->ops = &ext2_dir_ops;
    } else {
        v->type = VNODE_FILE;
        v->ops = &ext2_file_ops;
    }
    v->size = pv->inode.size;
    v->priv = pv;
    return v;
}

/* ---- монтирование ---- */

vnode_t* ext2_mount(void) {
    if (!virtio_blk_available()) {
        printf("[ext2] no block device\n");
        return NULL;
    }

    /* Superblock на offset 1024 — это блок 1 при block_size>=1024.
       Читаем напрямую с сектора 2 (1024/512). */
    u8 sbbuf[1024];
    if (virtio_blk_read(2, sbbuf) != 0 || virtio_blk_read(3, sbbuf + 512) != 0) {
        printf("[ext2] failed to read superblock\n");
        return NULL;
    }
    memcpy(&sb, sbbuf, sizeof(sb));

    if (sb.magic != EXT2_MAGIC) {
        printf("[ext2] bad magic 0x%x (expected 0xEF53)\n", sb.magic);
        return NULL;
    }

    block_size = 1024 << sb.log_block_size;
    sectors_per_block = block_size / 512;
    num_groups = (sb.blocks_count + sb.blocks_per_group - 1) / sb.blocks_per_group;

    printf("[ext2] magic OK, block_size=%u, %u inodes, %u blocks, %u groups\n",
           block_size, sb.inodes_count, sb.blocks_count, num_groups);

    /* Block group descriptor table — сразу после superblock.
       При block_size==1024 superblock в блоке 1, BGDT в блоке 2.
       При больших block_size BGDT в блоке 1. */
    u32 bgdt_block = (block_size == 1024) ? 2 : 1;
    u32 bgdt_size = num_groups * sizeof(ext2_group_desc_t);
    u32 bgdt_blocks = (bgdt_size + block_size - 1) / block_size;

    group_desc = (ext2_group_desc_t*)kmalloc(bgdt_blocks * block_size);
    if (!group_desc) return NULL;
    for (u32 i = 0; i < bgdt_blocks; i++) {
        if (read_block(bgdt_block + i, (u8*)group_desc + i * block_size) != 0) {
            printf("[ext2] failed to read BGDT\n");
            kfree(group_desc);
            return NULL;
        }
    }

    ext2_ready = 1;

    vnode_t* root = ext2_make_vnode(EXT2_ROOT_INO);
    ext2_root_vnode = root;
    if (!root) {
        printf("[ext2] failed to read root inode\n");
        return NULL;
    }
    printf("[ext2] mounted, root inode size=%u\n", (unsigned)root->size);
    return root;
}

int ext2_available(void) { return ext2_ready; }
