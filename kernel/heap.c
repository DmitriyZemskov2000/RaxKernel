/*
 * heap.c — kernel heap, kmalloc/kfree произвольных размеров.
 *
 * Дизайн:
 *   * При инициализации выделяем у PMM N страниц подряд и формируем
 *     из них один большой "пустой" блок.
 *   * Каждый блок имеет header'ом структуру block_t (размер + признак
 *     "свободен" + указатели на соседей в свободном списке).
 *   * kmalloc: ищем первый подходящий свободный блок (first-fit),
 *     если он сильно больше — режем пополам.
 *   * kfree: помечаем свободным, сливаем с соседями физически (через
 *     соседство по адресу), если они тоже свободны.
 *
 * Это не slab и не buddy. Производительность first-fit при тысячах
 * аллокаций может проседать. Но для ядра на этом этапе хватит за глаза;
 * заменим, когда станет узким местом.
 *
 * Расширение пула: пока не реализовано — выделяем сразу 1 MiB. Когда
 * упрёмся, переделаем на ленивое выделение страниц через VMM.
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include "pmm.h"
#include "heap.h"

#define HEAP_PAGES 8192         /* 8192 * 4 KiB = 32 MiB */
#define PAGE_SIZE  4096
#define ALIGN_TO   16ULL        /* malloc возвращает 16-байт выровненные адреса */

#define MAGIC_USED  0xA110CABEBABE0000ULL
#define MAGIC_FREE  0xF1EEB10C0DEC0DECULL

typedef struct block {
    u64 magic;
    size_t size;                /* размер payload, без заголовка */
    struct block* prev_phys;
    struct block* next_phys;
    struct block* prev_free;    /* связный список свободных */
    struct block* next_free;
} block_t;

static block_t* free_head = NULL;
static u8* heap_base = NULL;
static size_t heap_size = 0;

static inline size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

/* ---------- Свободный список ---------- */

static void freelist_push(block_t* b) {
    b->prev_free = NULL;
    b->next_free = free_head;
    if (free_head) free_head->prev_free = b;
    free_head = b;
}

static void freelist_remove(block_t* b) {
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else              free_head = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    b->prev_free = b->next_free = NULL;
}

/* ---------- Инициализация ---------- */

void heap_init(void) {
    /*
     * Выделяем HEAP_PAGES физических страниц (возможно разрозненных)
     * и мапим их на НЕПРЕРЫВНЫЙ виртуальный диапазон через VMM.
     * Это решает проблему физической фрагментации — heap всегда
     * получает полный размер.
     *
     * Виртуальный базовый адрес: 0xFFFFFFFF90000000 — в higher-half,
     * выше kernel image (который на 0xFFFFFFFF80000000 + до 64 MiB).
     */
    extern int vmm_map(u64 vaddr, u64 paddr, u64 flags);
    const u64 HEAP_VBASE = 0xFFFFFFFF90000000ULL;

    for (int i = 0; i < HEAP_PAGES; i++) {
        void* p = pmm_alloc_page();
        if (!p) { printf("[heap] кончилась память на странице %d\n", i); 
                  heap_size = (size_t)i * PAGE_SIZE; goto init_done; }
        u64 vaddr = HEAP_VBASE + (u64)i * PAGE_SIZE;
        if (vmm_map(vaddr, (u64)p, 0x2) != 0) {   /* WRITABLE, kernel-only */
            printf("[heap] vmm_map fail на стр %d\n", i);
            heap_size = (size_t)i * PAGE_SIZE; goto init_done;
        }
    }
    heap_size = HEAP_PAGES * PAGE_SIZE;

init_done:
    heap_base = (u8*)HEAP_VBASE;

    block_t* b = (block_t*)heap_base;
    b->magic = MAGIC_FREE;
    b->size  = heap_size - sizeof(block_t);
    b->prev_phys = b->next_phys = NULL;
    b->prev_free = b->next_free = NULL;
    free_head = b;

    printf("[heap] base=%p size=%lu KiB (virtually contiguous via VMM)\n",
           heap_base, heap_size / 1024);
}

/* ---------- kmalloc/kfree ---------- */

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = align_up(size, ALIGN_TO);

    /* Ищем first-fit. */
    for (block_t* b = free_head; b; b = b->next_free) {
        if (b->magic != MAGIC_FREE) {
            printf("[heap] CORRUPTION: free block w/o magic at %p\n", b);
            return NULL;
        }
        if (b->size < size) continue;

        /* Достаточно большой. Если запас велик — режем. */
        size_t leftover = b->size - size;
        if (leftover > sizeof(block_t) + ALIGN_TO) {
            /* Создаём новый свободный блок справа от выделяемого. */
            block_t* right = (block_t*)((u8*)b + sizeof(block_t) + size);
            right->magic = MAGIC_FREE;
            right->size  = leftover - sizeof(block_t);
            right->prev_phys = b;
            right->next_phys = b->next_phys;
            if (b->next_phys) b->next_phys->prev_phys = right;
            b->next_phys = right;
            b->size = size;
            freelist_push(right);
        }
        freelist_remove(b);
        b->magic = MAGIC_USED;
        return (u8*)b + sizeof(block_t);
    }
    printf("[heap] OOM kmalloc(%lu)\n", size);
    return NULL;
}

void kfree(void* ptr) {
    if (!ptr) return;
    block_t* b = (block_t*)((u8*)ptr - sizeof(block_t));
    if (b->magic != MAGIC_USED) {
        printf("[heap] double-free or bad ptr: %p (magic %lx)\n", ptr, b->magic);
        return;
    }
    b->magic = MAGIC_FREE;
    freelist_push(b);

    /* Сливаем с правым соседом, если свободен. */
    if (b->next_phys && b->next_phys->magic == MAGIC_FREE) {
        block_t* r = b->next_phys;
        freelist_remove(r);
        b->size += sizeof(block_t) + r->size;
        b->next_phys = r->next_phys;
        if (r->next_phys) r->next_phys->prev_phys = b;
    }
    /* И с левым. */
    if (b->prev_phys && b->prev_phys->magic == MAGIC_FREE) {
        block_t* l = b->prev_phys;
        freelist_remove(b);
        l->size += sizeof(block_t) + b->size;
        l->next_phys = b->next_phys;
        if (b->next_phys) b->next_phys->prev_phys = l;
    }
}

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }
    block_t* b = (block_t*)((u8*)ptr - sizeof(block_t));
    if (b->magic != MAGIC_USED) {
        printf("[heap] realloc on bad ptr\n");
        return NULL;
    }
    if (b->size >= new_size) return ptr;  /* shrink — пока не делим */

    void* np = kmalloc(new_size);
    if (!np) return NULL;
    memcpy(np, ptr, b->size);
    kfree(ptr);
    return np;
}

void heap_dump_stats(void) {
    size_t total_free = 0;
    int n_free = 0;
    for (block_t* b = free_head; b; b = b->next_free) {
        total_free += b->size;
        n_free++;
    }
    printf("[heap] free blocks: %d, total free payload: %lu bytes\n",
            n_free, total_free);
}
