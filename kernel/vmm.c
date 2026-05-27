/*
 * vmm.c — virtual memory manager.
 *
 * Задачи:
 *   * Построить и поддерживать иерархию page tables (PML4 → PDPT → PD → PT).
 *   * Маппить произвольную физическую страницу 4 KiB в произвольный
 *     виртуальный адрес с заданными правами (W, U, NX).
 *   * Снимать маппинг и инвалидировать TLB.
 *   * Переводить виртуальный адрес в физический.
 *   * Создавать новые адресные пространства (для будущих процессов).
 *
 * Архитектурные ограничения x86_64 paging:
 *   * Каждый уровень — таблица из 512 записей по 8 байт = 4 KiB.
 *   * Индексы из виртуального адреса:
 *       биты 39..47 — PML4 index
 *       биты 30..38 — PDPT index
 *       биты 21..29 — PD index
 *       биты 12..20 — PT index
 *       биты  0..11 — offset
 *   * Каноничность: биты 48..63 должны равняться биту 47.
 *     Half-space split: нижняя половина 0x0000000000000000..0x00007FFFFFFFFFFF
 *                       верхняя половина 0xFFFF800000000000..0xFFFFFFFFFFFFFFFF.
 *
 * Что НЕ делает эта итерация:
 *   * Не работает с huge pages (бутстрап их использует, но мы их не трогаем).
 *   * Не делает SMP-инвалидацию TLB. У нас один CPU.
 *   * Нет copy-on-write — это нужно для fork(), который пока не нужен.
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include "pmm.h"
#include "vmm.h"

#define PAGE_SIZE       4096ULL
#define PAGE_MASK       (PAGE_SIZE - 1)
#define ENTRIES_PER_TBL 512

/* Флаги PTE */
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITABLE    (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_WT          (1ULL << 3)   /* write-through */
#define PTE_CD          (1ULL << 4)   /* cache-disable */
#define PTE_ACCESSED    (1ULL << 5)
#define PTE_DIRTY       (1ULL << 6)
#define PTE_HUGE        (1ULL << 7)   /* PS bit: 2 MiB на уровне PD, 1 GiB на PDPT */
#define PTE_GLOBAL      (1ULL << 8)
#define PTE_NX          (1ULL << 63)

/* Маска физического адреса внутри PTE: биты 12..51 */
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/*
 * Внутреннее представление текущего PML4. Изначально — physical address
 * того PML4, что построил бутстрап. После vmm_init мы создаём свой
 * (с identity + higher-half), переключаем CR3 и забываем про бутстрап.
 */
static u64 kernel_pml4_phys = 0;

/* ---------- TLB и CR3 ---------- */

static inline void invlpg(u64 vaddr) {
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static inline u64 read_cr3(void) {
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(u64 v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

/* ---------- Доступ к таблицам ----------
 *
 * До настоящего kernel mapping'а первый 1 GiB у нас identity-mapped.
 * Поэтому физический адрес < 1 GiB == виртуальный адрес. Этим
 * пользуемся: интерпретируем phys как указатель напрямую.
 *
 * После того как kernel переедет в higher-half (итерация 3+) — здесь
 * появится физ→вирт-преобразование через offset 0xFFFF8000_00000000.
 */
static inline u64* phys_to_table(u64 phys) {
    return (u64*)(uintptr_t)phys;
}

static inline u64 vaddr_index(u64 vaddr, int level) {
    /* level: 4 = PML4, 3 = PDPT, 2 = PD, 1 = PT */
    int shift = 12 + (level - 1) * 9;
    return (vaddr >> shift) & 0x1FF;
}

/*
 * Заглядываем/создаём таблицу следующего уровня.
 * Если PTE отсутствует и create=1 — выделяем новую страницу,
 * обнуляем, прописываем в parent[idx] флаги P|W|U.
 * Возвращает физический адрес дочерней таблицы, либо 0.
 */
static u64 next_table(u64* parent, u64 idx, int create, u64 user_flag) {
    u64 entry = parent[idx];
    if (entry & PTE_PRESENT) {
        /* Если страница huge (PS=1), это не таблица. Раньше мы возвращали 0
           (не могли разделить), из-за чего нельзя было замапить 4 KiB
           USER-страницу внутри бутстрапного 2 MiB huge-региона (нижний
           1 GiB) — и кросс-бинари, слинкованные на 0x400000, падали с
           page fault в ring3. Теперь РАЗДЕЛЯЕМ huge на 512 обычных 4 KiB
           страниц, сохраняя то же отображение, и добавляем USER при нужде. */
        if (entry & PTE_HUGE) {
            if (!create) return 0;
            u64 base_phys = entry & PTE_ADDR_MASK;     /* 2 MiB-выровненный */
            u64 old_flags = entry & (PTE_WRITABLE | PTE_USER);
            void* pt_page = pmm_alloc_page();
            if (!pt_page) return 0;
            u64* pt = (u64*)pt_page;
            for (int i = 0; i < 512; i++) {
                pt[i] = (base_phys + (u64)i * PAGE_SIZE)
                        | PTE_PRESENT | old_flags | user_flag;
            }
            u64 pt_phys = (u64)(uintptr_t)pt_page;
            /* Заменяем huge-запись на указатель на новую таблицу (без PS). */
            parent[idx] = pt_phys | PTE_PRESENT | PTE_WRITABLE | user_flag;
            return pt_phys;
        }
        /* Если запрошен USER, а в существующей записи его нет — добавим. */
        if (user_flag && !(entry & PTE_USER)) {
            parent[idx] = entry | PTE_USER;
        }
        return entry & PTE_ADDR_MASK;
    }
    if (!create) return 0;

    void* page = pmm_alloc_page();
    if (!page) return 0;
    memset(page, 0, PAGE_SIZE);
    u64 phys = (u64)(uintptr_t)page;
    parent[idx] = phys | PTE_PRESENT | PTE_WRITABLE | user_flag;
    return phys;
}

/* ---------- Публичный API ---------- */

int vmm_map(u64 vaddr, u64 paddr, u64 flags) {
    if (vaddr & PAGE_MASK || paddr & PAGE_MASK) return -1;

    u64 user_flag = (flags & PTE_USER) ? PTE_USER : 0;
    /* Мапим в ТЕКУЩИЙ address space (CR3), а не всегда в kernel_pml4.
       Иначе execve в forked-процессе затирал бы маппинги родителя. */
    u64* pml4 = phys_to_table(read_cr3() & ~0xFFFULL);

    u64 pdpt_phys = next_table(pml4, vaddr_index(vaddr, 4), 1, user_flag);
    if (!pdpt_phys) return -1;
    u64* pdpt = phys_to_table(pdpt_phys);

    u64 pd_phys = next_table(pdpt, vaddr_index(vaddr, 3), 1, user_flag);
    if (!pd_phys) return -1;
    u64* pd = phys_to_table(pd_phys);

    u64 pt_phys = next_table(pd, vaddr_index(vaddr, 2), 1, user_flag);
    if (!pt_phys) return -1;
    u64* pt = phys_to_table(pt_phys);

    u64 idx = vaddr_index(vaddr, 1);
    pt[idx] = (paddr & PTE_ADDR_MASK) | (flags & 0xFFF) | PTE_PRESENT;
    if (flags & VMM_NX) pt[idx] |= PTE_NX;

    invlpg(vaddr);
    return 0;
}

int vmm_unmap(u64 vaddr) {
    if (vaddr & PAGE_MASK) return -1;

    u64* pml4 = phys_to_table(kernel_pml4_phys);
    u64 pdpt_phys = next_table(pml4, vaddr_index(vaddr, 4), 0, 0);
    if (!pdpt_phys) return -1;
    u64* pdpt = phys_to_table(pdpt_phys);
    u64 pd_phys = next_table(pdpt, vaddr_index(vaddr, 3), 0, 0);
    if (!pd_phys) return -1;
    u64* pd = phys_to_table(pd_phys);
    u64 pt_phys = next_table(pd, vaddr_index(vaddr, 2), 0, 0);
    if (!pt_phys) return -1;
    u64* pt = phys_to_table(pt_phys);

    u64 idx = vaddr_index(vaddr, 1);
    if (!(pt[idx] & PTE_PRESENT)) return -1;
    pt[idx] = 0;
    invlpg(vaddr);
    /* Пустые промежуточные таблицы мы пока не освобождаем —
       это потребовало бы счётчиков использования. Цена — лишние
       4 KiB на каждый "когда-то использованный, но потом пустой"
       PML4-слот. Для ядра это копейки. */
    return 0;
}

u64 vmm_translate(u64 vaddr) {
    u64* pml4 = phys_to_table(kernel_pml4_phys);
    u64 idx = vaddr_index(vaddr, 4);
    if (!(pml4[idx] & PTE_PRESENT)) return 0;
    u64 pdpt_phys = pml4[idx] & PTE_ADDR_MASK;
    u64* pdpt = phys_to_table(pdpt_phys);

    idx = vaddr_index(vaddr, 3);
    if (!(pdpt[idx] & PTE_PRESENT)) return 0;
    if (pdpt[idx] & PTE_HUGE) {
        /* 1 GiB huge */
        return (pdpt[idx] & 0x000FFFFFC0000000ULL) | (vaddr & 0x3FFFFFFF);
    }
    u64 pd_phys = pdpt[idx] & PTE_ADDR_MASK;
    u64* pd = phys_to_table(pd_phys);

    idx = vaddr_index(vaddr, 2);
    if (!(pd[idx] & PTE_PRESENT)) return 0;
    if (pd[idx] & PTE_HUGE) {
        /* 2 MiB huge (это наш случай в нижнем 1 GiB) */
        return (pd[idx] & 0x000FFFFFFFE00000ULL) | (vaddr & 0x1FFFFF);
    }
    u64 pt_phys = pd[idx] & PTE_ADDR_MASK;
    u64* pt = phys_to_table(pt_phys);

    idx = vaddr_index(vaddr, 1);
    if (!(pt[idx] & PTE_PRESENT)) return 0;
    return (pt[idx] & PTE_ADDR_MASK) | (vaddr & PAGE_MASK);
}

/* ---------- Создание нового адресного пространства ----------
 *
 * Возвращает физ. адрес нового PML4, в котором заполнены
 * верхние 256 записей (kernel space) — копией из текущего.
 * Нижние 256 записей (userspace) обнулены.
 *
 * Это стандартный приём: все процессы делят kernel-маппинги
 * через ту же половину PML4, чтобы syscall не требовал
 * переключения CR3.
 */
u64 vmm_create_space(void) {
    void* p = pmm_alloc_page();
    if (!p) return 0;
    memset(p, 0, PAGE_SIZE);
    u64 phys = (u64)(uintptr_t)p;

    u64* dst = phys_to_table(phys);
    u64* src = phys_to_table(kernel_pml4_phys);

    /* Копируем верхнюю половину (записи 256..511) — kernel space. */
    for (int i = 256; i < 512; i++) {
        dst[i] = src[i];
    }
    return phys;
}

void vmm_switch_space(u64 pml4_phys) {
    write_cr3(pml4_phys);
}

/* ---------- Инициализация ----------
 *
 * Шаги:
 *  1) Создаём новый PML4 в свободной памяти от PMM.
 *  2) Воспроизводим identity-маппинг первого 1 GiB через 4 KiB страницы
 *     (бутстрапные 2 MiB huge — оставляем работать "пока что", но новый
 *     PML4 строим уже на 4 KiB страницах, чтобы дальше было гибко).
 *  3) Дублируем тот же первый 1 GiB в higher-half (0xFFFFFFFF80000000).
 *  4) Переключаем CR3 на новый PML4.
 *
 * Внимание: пока мы строим новый PML4, мы ещё пользуемся старым,
 * поэтому функция next_table() работает через старый CR3. Это ок —
 * она пишет в физические страницы напрямую через identity-mapping.
 */

/* Адрес higher-half базы */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL

void vmm_init(void) {
    /* На старте используем boot_p4_table — он был построен ASM-частью.
       Мы можем тут же создать собственный PML4, но это сложнее: придётся
       аккуратно перехватить выполнение. Проще — сначала "взять под себя"
       тот PML4, что есть, а затем уже из него произвести нужные
       дополнительные маппинги.

       Но boot_p4_table содержит huge pages 2 MiB. Если мы захотим
       мапить отдельные 4 KiB страницы внутри уже замапленного 2 MiB
       региона, придётся сначала "разбить" huge на обычные. В этой
       итерации мы такого ещё не делаем — все наши новые маппинги
       пойдут в адреса вне нижнего 1 GiB (то есть в higher-half через
       PT, который мы добавим). */

    extern char boot_p4_table[];
    kernel_pml4_phys = (u64)(uintptr_t)boot_p4_table;

    /* Дополнительно мапим higher-half: KERNEL_VIRT_BASE → phys 0,
       первые, скажем, 64 MiB. Через 4 KiB страницы. Это даст нам
       альтернативные виртуальные адреса для ядра — пригодится в
       итерации 3, когда мы перелинкуем ядро в higher-half. */
    const u64 mb = 1024ULL * 1024ULL;
    const u64 region = 64 * mb;
    u64 mapped = 0;
    for (u64 off = 0; off < region; off += PAGE_SIZE) {
        if (vmm_map(KERNEL_VIRT_BASE + off, off, PTE_WRITABLE) == 0) {
            mapped++;
        } else {
            printf("[vmm] mapping failed at offset 0x%lx\n", off);
            break;
        }
    }

    printf("[vmm] higher-half mapped: %lu pages (%lu MiB) at 0x%lx\n",
            mapped, (mapped * PAGE_SIZE) / mb, KERNEL_VIRT_BASE);

    /* Self-test: проверим, что виртуальный higher-half адрес действительно
       указывает на тот же физический байт, что и identity. */
    u64 test_vaddr = KERNEL_VIRT_BASE + 0xB8000;
    u64 phys = vmm_translate(test_vaddr);
    printf("[vmm] translate(0x%lx) = 0x%lx (ожидалось 0xb8000)\n",
            test_vaddr, phys);

    /* Проверка записи через higher-half — пишем символ в VGA через
       higher-half указатель и убеждаемся, что он появился. */
    volatile u16* vga_hi = (volatile u16*)(test_vaddr);
    vga_hi[80] = (u16)('!') | (0x2F << 8);    /* красивый зелёный фон */

    printf("[vmm] higher-half write test ok (см. 2-ю строку экрана)\n");
}

/*
 * vmm_clone_address_space — копирует **весь current address space**:
 *   - kernel half (>= 256-я запись PML4) копируется ссылкой (shared)
 *   - user half (0..256-я запись PML4) копируется глубоко:
 *     каждая user-страница physically duplicated, маппинг воссоздан в новом PML4
 *
 * Это нужно для fork: child должен видеть точную копию памяти parent
 * (на момент fork), но дальше его модификации не должны затрагивать parent.
 *
 * Без COW: дороже, но правильно. Размер copy = размер user-памяти процесса.
 *
 * Ходим по дереву PML4 → PDPT → PD → PT и для каждой leaf PTE с PRESENT
 * аллоцируем новую физ. страницу, копируем 4 KiB, мапим в новом PML4.
 */
static void clone_pt(u64* src_pt, u64 dst_phys_pt, u64 user_flag) {
    u64* dst_pt = phys_to_table(dst_phys_pt);
    for (int i = 0; i < 512; i++) {
        u64 src_pte = src_pt[i];
        if (!(src_pte & PTE_PRESENT)) continue;
        u64 src_phys = src_pte & PTE_ADDR_MASK;
        u64 flags = src_pte & 0xFFF;

        /* Аллоцируем новую физ. страницу и копируем */
        void* new_phys_p = pmm_alloc_page();
        if (!new_phys_p) {
#ifdef SYSCALL_DEBUG
            extern int printf(const char*, ...);
            printf("[clone OOM!]");
#endif
            continue;
        }
        u64 new_phys = (u64)(uintptr_t)new_phys_p;
        memcpy(phys_to_table(new_phys), phys_to_table(src_phys), PAGE_SIZE);
        dst_pt[i] = new_phys | flags;
    }
    (void)user_flag;
}

static void clone_pd(u64* src_pd, u64 dst_phys_pd, u64 user_flag) {
    u64* dst_pd = phys_to_table(dst_phys_pd);
    for (int i = 0; i < 512; i++) {
        u64 src_pde = src_pd[i];
        if (!(src_pde & PTE_PRESENT)) continue;
        if (src_pde & PTE_HUGE) {
            /* 2 MiB страницы — у нас они используются только для
               identity-mapping бутстрапа, не для пользователя.
               Копируем ссылкой (shared). */
            dst_pd[i] = src_pde;
            continue;
        }
        void* new_pt_p = pmm_alloc_page();
        if (!new_pt_p) continue;
        memset(new_pt_p, 0, PAGE_SIZE);
        u64 new_pt_phys = (u64)(uintptr_t)new_pt_p;
        clone_pt(phys_to_table(src_pde & PTE_ADDR_MASK), new_pt_phys, user_flag);
        u64 flags = src_pde & 0xFFF;
        dst_pd[i] = new_pt_phys | flags;
    }
}

static void clone_pdpt(u64* src_pdpt, u64 dst_phys_pdpt, u64 user_flag) {
    u64* dst_pdpt = phys_to_table(dst_phys_pdpt);
    for (int i = 0; i < 512; i++) {
        u64 src_pdpte = src_pdpt[i];
        if (!(src_pdpte & PTE_PRESENT)) continue;
        if (src_pdpte & PTE_HUGE) {
            dst_pdpt[i] = src_pdpte;
            continue;
        }
        void* new_pd_p = pmm_alloc_page();
        if (!new_pd_p) continue;
        memset(new_pd_p, 0, PAGE_SIZE);
        u64 new_pd_phys = (u64)(uintptr_t)new_pd_p;
        clone_pd(phys_to_table(src_pdpte & PTE_ADDR_MASK), new_pd_phys, user_flag);
        u64 flags = src_pdpte & 0xFFF;
        dst_pdpt[i] = new_pd_phys | flags;
    }
}

u64 vmm_clone_address_space(void) {
    u64 new_pml4_phys = vmm_create_space();   /* верхняя половина уже скопирована shared */
    if (!new_pml4_phys) return 0;

    /* Клонируем из ТЕКУЩЕГО address space (read_cr3), а не из
       kernel_pml4_phys — иначе fork от forked-процесса (напр. chibicc,
       запущенный через execve и форкающий снова) не увидит свои
       страницы кода/данных. */
    u64 cur_cr3 = read_cr3() & ~0xFFFULL;
    u64* src = phys_to_table(cur_cr3);
    u64* dst = phys_to_table(new_pml4_phys);

    /* User half: записи 0..255 — клонируем глубоко. */
    for (int i = 0; i < 256; i++) {
        u64 src_pml4e = src[i];
        if (!(src_pml4e & PTE_PRESENT)) continue;

        /* Только USER записи копируем. Если запись существует но без USER —
           это бутстрапный identity-mapping (нижний 1 GiB), копируем shared. */
        if (!(src_pml4e & PTE_USER)) {
            dst[i] = src_pml4e;
            continue;
        }

        void* new_pdpt_p = pmm_alloc_page();
        if (!new_pdpt_p) continue;
        memset(new_pdpt_p, 0, PAGE_SIZE);
        u64 new_pdpt_phys = (u64)(uintptr_t)new_pdpt_p;
        clone_pdpt(phys_to_table(src_pml4e & PTE_ADDR_MASK), new_pdpt_phys, PTE_USER);
        u64 flags = src_pml4e & 0xFFF;
        dst[i] = new_pdpt_phys | flags;
    }

    return new_pml4_phys;
}

/* Текущий CR3 — для fork нам нужно знать какой PML4 у текущей задачи */
u64 vmm_current_cr3(void) {
    return read_cr3();
}

u64 vmm_kernel_cr3(void) {
    return kernel_pml4_phys;
}
