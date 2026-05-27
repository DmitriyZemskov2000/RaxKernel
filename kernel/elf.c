/*
 * elf.c — загрузчик ELF64.
 *
 * Заменяет raw blob loader из main.c. Читает program headers,
 * мапит каждый LOAD segment с правильным размером (filesz < memsz
 * = bss zero-fill), и возвращает entry point.
 *
 * Этого хватит для запуска: hello, busybox-style утилит,
 * и в будущем — gcc/binutils (когда добьём остальную инфраструктуру).
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include "elf.h"
#include "pmm.h"
#include "vmm.h"
#include "vfs.h"

typedef struct {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} __attribute__((packed)) Elf64_Phdr;

#define PT_LOAD     1
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

/* elf_load_fd — загружает ELF напрямую из открытого fd, читая сегменты
   с диска постранично. НЕ требует держать весь файл в kernel heap —
   это позволяет грузить большие бинари (напр. cc1 207 МБ), которые не
   влезают в 32 МБ kernel heap. */
int elf_load_fd(int fd, u64* out_entry) {
    extern ssize_t vfs_read(int, void*, size_t);
    extern off_t   vfs_lseek(int, off_t, int);

    Elf64_Ehdr eh;
    if (vfs_lseek(fd, 0, 0) < 0) return -1;
    if (vfs_read(fd, &eh, sizeof(eh)) != (ssize_t)sizeof(eh)) return -1;

    if (eh.e_ident[0] != ELF_MAGIC0 || eh.e_ident[1] != ELF_MAGIC1 ||
        eh.e_ident[2] != ELF_MAGIC2 || eh.e_ident[3] != ELF_MAGIC3) {
        printf("[elf] bad magic\n");
        return -1;
    }
    if (eh.e_ident[4] != 2) { printf("[elf] not 64-bit\n"); return -1; }
    if (eh.e_type != 2 && eh.e_type != 3) {
        printf("[elf] not executable (type=%d)\n", eh.e_type);
        return -1;
    }
    if (eh.e_phnum > 64) { printf("[elf] too many phdrs\n"); return -1; }

    printf("[elf] entry=0x%lx, %d program headers (streaming)\n",
           eh.e_entry, eh.e_phnum);

    /* Читаем program headers (маленькие — до 64*56 байт) */
    Elf64_Phdr ph_table[64];
    if (vfs_lseek(fd, (off_t)eh.e_phoff, 0) < 0) return -1;
    size_t ph_bytes = (size_t)eh.e_phnum * sizeof(Elf64_Phdr);
    if (vfs_read(fd, ph_table, ph_bytes) != (ssize_t)ph_bytes) return -1;

    for (int i = 0; i < eh.e_phnum; i++) {
        const Elf64_Phdr* ph = &ph_table[i];
        if (ph->p_type != PT_LOAD) continue;

        u64 vaddr_start = ph->p_vaddr & ~4095ULL;
        u64 vaddr_end   = (ph->p_vaddr + ph->p_memsz + 4095) & ~4095ULL;

        printf("[elf]   LOAD 0x%lx..0x%lx filesz=%lu memsz=%lu %c%c%c\n",
               vaddr_start, vaddr_end, ph->p_filesz, ph->p_memsz,
               (ph->p_flags & PF_R) ? 'r' : '-',
               (ph->p_flags & PF_W) ? 'w' : '-',
               (ph->p_flags & PF_X) ? 'x' : '-');

        /* Маппим страницы сегмента */
        u64 flags = VMM_WRITABLE | VMM_USER;
        for (u64 a = vaddr_start; a < vaddr_end; a += 4096) {
            void* phys = pmm_alloc_page();
            if (!phys) { printf("[elf] oom @0x%lx\n", a); return -1; }
            if (vmm_map(a, (u64)phys, flags) != 0) {
                pmm_free_page(phys);
            }
            memset((void*)a, 0, 4096);
        }

        /* Читаем filesz байт сегмента ПРЯМО С ДИСКА в user-память по
           p_vaddr. Читаем кусками, чтобы не зависеть от размера. */
        if (ph->p_filesz > 0) {
            if (vfs_lseek(fd, (off_t)ph->p_offset, 0) < 0) return -1;
            u64  dst = ph->p_vaddr;
            u64  remaining = ph->p_filesz;
            while (remaining > 0) {
                size_t chunk = remaining > 65536 ? 65536 : (size_t)remaining;
                ssize_t got = vfs_read(fd, (void*)dst, chunk);
                if (got <= 0) { printf("[elf] read fail @0x%lx\n", dst); return -1; }
                dst += (u64)got;
                remaining -= (u64)got;
            }
        }
    }

    if (out_entry) *out_entry = eh.e_entry;
    return 0;
}

int elf_load(const void* image, size_t image_size, u64* out_entry) {
    if (image_size < sizeof(Elf64_Ehdr)) return -1;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)image;

    if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
        eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) {
        printf("[elf] bad magic\n");
        return -1;
    }
    if (eh->e_ident[4] != 2) { printf("[elf] not 64-bit\n"); return -1; }
    if (eh->e_type != 2 && eh->e_type != 3) {
        printf("[elf] not executable (type=%d)\n", eh->e_type);
        return -1;
    }

    printf("[elf] entry=0x%lx, %d program headers\n", eh->e_entry, eh->e_phnum);

    const u8* base = (const u8*)image;
    const Elf64_Phdr* ph_table = (const Elf64_Phdr*)(base + eh->e_phoff);

    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* ph = &ph_table[i];
        if (ph->p_type != PT_LOAD) continue;

        u64 vaddr_start = ph->p_vaddr & ~4095ULL;
        u64 vaddr_end   = (ph->p_vaddr + ph->p_memsz + 4095) & ~4095ULL;

        printf("[elf]   LOAD vaddr=0x%lx..0x%lx filesz=%lu memsz=%lu flags=%c%c%c\n",
               vaddr_start, vaddr_end,
               ph->p_filesz, ph->p_memsz,
               (ph->p_flags & PF_R) ? 'r' : '-',
               (ph->p_flags & PF_W) ? 'w' : '-',
               (ph->p_flags & PF_X) ? 'x' : '-');

        /* Маппим страницы. На future-proof'инг разрешаем write только
           если флаг W (но сейчас всегда WRITABLE, чтобы memcpy сработал). */
        u64 flags = VMM_WRITABLE | VMM_USER;
        for (u64 a = vaddr_start; a < vaddr_end; a += 4096) {
            void* phys = pmm_alloc_page();
            if (!phys) { printf("[elf] oom\n"); return -1; }
            if (vmm_map(a, (u64)phys, flags) != 0) {
                /* Возможно страница уже замаплена (overlap между LOAD segments).
                   В этом случае просто продолжаем. */
                pmm_free_page(phys);
            }
            memset((void*)a, 0, 4096);
        }
        /* Копируем filesz байт по vaddr (не align'ed). Остальное (memsz -
           filesz) — это .bss, остаётся нулём благодаря memset выше. */
        if (ph->p_filesz > 0) {
            memcpy((void*)ph->p_vaddr, base + ph->p_offset, ph->p_filesz);
        }
    }

    if (out_entry) *out_entry = eh->e_entry;
    return 0;
}
