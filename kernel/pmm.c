/*
 * pmm.c — physical memory manager (bitmap allocator).
 *
 * Зачем: чтобы что-то динамически выделять (структуры ядра, буферы,
 * page tables для нового адресного пространства), нужен пул свободных
 * физических страниц. Самая простая работающая реализация — bitmap:
 * 1 бит = 1 страница 4 KiB. Для 4 GiB ОЗУ это 128 KiB битмапы — недорого.
 *
 * Multiboot2 даёт нам карту памяти (какие куски физического пространства
 * валидны как RAM, а какие занимает MMIO/ACPI). Мы её парсим, помечаем
 * RAM-страницы как свободные и затем помечаем как занятые те, что уже
 * использует ядро (его собственный образ).
 *
 * Это не финальный аллокатор. Финальный будет buddy / slab, но bitmap
 * хорош как стартовая точка: 100 строк, легко отлаживается, работает.
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include "pmm.h"

#define PAGE_SIZE  4096
#define MAX_PAGES  (1ULL << 20)         /* поддерживаем до 4 GiB */
#define BITMAP_SZ  (MAX_PAGES / 8)      /* 128 KiB */

/* Битмап в .bss — обнулится автоматически. 1 = свободна, 0 = занята.
   ВНИМАНИЕ: мы сначала всё помечаем как занятое (calloc-эффект),
   потом проходимся по карте Multiboot и помечаем RAM как свободное. */
static u8 bitmap[BITMAP_SZ];
static u64 total_pages = 0;
static u64 free_pages = 0;

static inline void bm_set(u64 page)   { bitmap[page / 8] |=  (1 << (page % 8)); }
static inline void bm_clear(u64 page) { bitmap[page / 8] &= ~(1 << (page % 8)); }
static inline int  bm_test(u64 page)  { return bitmap[page / 8] & (1 << (page % 8)); }

/* ---------- парсинг Multiboot2 ---------- */

typedef struct PACKED {
    u32 total_size;
    u32 reserved;
} mb2_header_t;

typedef struct PACKED {
    u32 type;
    u32 size;
} mb2_tag_t;

#define MB2_TAG_MMAP 6

typedef struct PACKED {
    u64 base_addr;
    u64 length;
    u32 type;       /* 1 = available RAM */
    u32 reserved;
} mb2_mmap_entry_t;

typedef struct PACKED {
    u32 type;
    u32 size;
    u32 entry_size;
    u32 entry_version;
    /* далее идут entry_size-байтовые mb2_mmap_entry_t */
} mb2_mmap_tag_t;

extern char _kernel_start[];
extern char _kernel_end[];

void pmm_init(void* multiboot_info) {
    /* Сначала всё занято */
    memset(bitmap, 0x00, sizeof(bitmap));
    total_pages = 0;
    free_pages  = 0;

    mb2_header_t* hdr = (mb2_header_t*)multiboot_info;
    u8* p = (u8*)multiboot_info + 8;
    u8* end = (u8*)multiboot_info + hdr->total_size;

    while (p < end) {
        mb2_tag_t* tag = (mb2_tag_t*)p;
        if (tag->type == 0) break;          /* end tag */

        if (tag->type == MB2_TAG_MMAP) {
            mb2_mmap_tag_t* mmap = (mb2_mmap_tag_t*)tag;
            u8* e = (u8*)mmap + 16;          /* первый entry */
            u8* eend = (u8*)mmap + mmap->size;
            while (e < eend) {
                mb2_mmap_entry_t* entry = (mb2_mmap_entry_t*)e;
                if (entry->type == 1) {
                    /* Доступная RAM. Освобождаем страницы. */
                    u64 start = (entry->base_addr + PAGE_SIZE - 1) / PAGE_SIZE;
                    u64 stop  = (entry->base_addr + entry->length) / PAGE_SIZE;
                    for (u64 pg = start; pg < stop && pg < MAX_PAGES; pg++) {
                        bm_set(pg);
                        free_pages++;
                    }
                    if (stop > total_pages && stop < MAX_PAGES) total_pages = stop;
                }
                e += mmap->entry_size;
            }
        }

        /* выравниваем размер до 8 байт */
        p += (tag->size + 7) & ~7ULL;
    }

    /* Резервируем нижний 1 MiB (BIOS, VGA, прочее железо) */
    for (u64 pg = 0; pg < 256; pg++) {
        if (bm_test(pg)) { bm_clear(pg); free_pages--; }
    }

    /* Резервируем сам образ ядра */
    u64 kstart = (u64)_kernel_start / PAGE_SIZE;
    u64 kend   = ((u64)_kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u64 pg = kstart; pg < kend; pg++) {
        if (bm_test(pg)) { bm_clear(pg); free_pages--; }
    }

    /* Также резервируем сам multiboot_info — нам нельзя его затирать
       пока мы храним указатель. */
    {
        mb2_header_t* h = (mb2_header_t*)multiboot_info;
        u64 mbi_start = (u64)multiboot_info / PAGE_SIZE;
        u64 mbi_end   = ((u64)multiboot_info + h->total_size + PAGE_SIZE - 1) / PAGE_SIZE;
        for (u64 pg = mbi_start; pg < mbi_end; pg++) {
            if (bm_test(pg)) { bm_clear(pg); free_pages--; }
        }
    }

    printf("[pmm] total: %lu pages (%lu MiB), free: %lu pages (%lu MiB)\n",
            total_pages, (total_pages * PAGE_SIZE) / (1024*1024),
            free_pages,  (free_pages  * PAGE_SIZE) / (1024*1024));
}

/* Резервирование произвольного физического диапазона. */
void pmm_reserve_range(u64 phys_start, u64 phys_end) {
    u64 pg_start = phys_start / PAGE_SIZE;
    u64 pg_end   = (phys_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u64 pg = pg_start; pg < pg_end && pg < total_pages; pg++) {
        if (bm_test(pg)) { bm_clear(pg); free_pages--; }
    }
}

void* pmm_alloc_page(void) {
    for (u64 pg = 1; pg < total_pages; pg++) {
        if (bm_test(pg)) {
            bm_clear(pg);
            free_pages--;
            /* первый GiB identity-mapped, так что физ. адрес == вирт. */
            return (void*)(pg * PAGE_SIZE);
        }
    }
    return NULL;
}

void pmm_free_page(void* addr) {
    u64 pg = (u64)addr / PAGE_SIZE;
    if (pg >= total_pages || bm_test(pg)) return;  /* уже свободна или вне */
    bm_set(pg);
    free_pages++;
}

u64 pmm_free_count(void) { return free_pages; }
u64 pmm_total_count(void) { return total_pages; }
