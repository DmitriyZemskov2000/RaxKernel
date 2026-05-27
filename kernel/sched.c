/*
 * sched.c — планировщик задач (round-robin).
 *
 * Архитектура:
 *
 *   task_t {
 *      uint64_t* rsp;        // указатель на сохранённый стек
 *      char*     stack;       // начало выделенного стека (для free)
 *      task_state state;      // RUNNING / READY / BLOCKED / DEAD
 *      char       name[16];
 *      list_node  link;       // в circular list "all tasks"
 *   }
 *
 * Все задачи лежат в кольцевом списке. current_task указывает на ту,
 * что сейчас исполняется. tick прерывание → schedule() → находим
 * следующую READY → switch_context.
 *
 * При создании задачи мы вручную раскладываем её начальный стек:
 *   [top]
 *     ret_addr = task_trampoline       ← сюда вернётся первый switch_context
 *     r15, r14, r13, r12, rbp, rbx     ← заглушки (0)
 *   [bottom]
 *
 * task_trampoline (на C) включает прерывания и вызывает task->entry(arg).
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "heap.h"
#include "pit.h"
#include "panic.h"
#include "sched.h"

/* ----- Конфиг ----- */
#define TASK_STACK_SIZE   (64 * 1024)        /* 64 KiB на задачу */
#define MAX_NAME          16
#define FXSAVE_SIZE       512

/* ----- Внутреннее ----- */
typedef enum { TS_READY, TS_RUNNING, TS_BLOCKED, TS_DEAD } task_state_t;

struct task {
    u64*          rsp;
    char*         stack;
    task_state_t  state;
    char          name[MAX_NAME];
    void          (*entry)(void*);
    void*         arg;
    struct task*  next;       /* circular */
    int           id;
    /* FPU/SSE state — 16-байт выровненный буфер. */
    u8            fxsave[FXSAVE_SIZE] __attribute__((aligned(16)));
    /* Поля для user mode: */
    int           is_user;            /* 1 если задача запущена в ring 3 */
    char*         kstack;             /* отдельный kernel-stack для этой user-задачи */
    u64           kstack_top;          /* RSP0, кладётся в TSS при выборе задачи */
    u64           cr3;                  /* физ. адрес PML4 */
    void*         process;              /* process_t* (если user task) */
    u64           saved_user_rsp;       /* percpu.user_rsp_save этой задачи */
    u64           fs_base;              /* база FS-сегмента (TLS, arch_prctl) */
};

extern void switch_context(u64** old_rsp_ptr, u64* new_rsp,
                           void* old_fxsave, void* new_fxsave);

static struct task* current = NULL;
static struct task* task_list_head = NULL;   /* любой узел кольца */
static int next_id = 1;
static volatile int sched_enabled = 0;

/* Idle-задача, всегда READY, чтобы при отсутствии других было что выполнять. */
static struct task* idle_task = NULL;

/* ---------- Утилиты ---------- */

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }

static inline u64 save_irq(void) {
    u64 f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f)::"memory");
    return f;
}
static inline void restore_irq(u64 f) {
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory","cc");
}

/* ---------- Trampoline ----------
 * После первого switch_context'а на новую задачу мы возвращаемся
 * сюда. Включаем прерывания (важно — иначе таймер не сможет
 * прервать эту задачу) и зовём её entry-функцию. */
static void task_trampoline(void) {
    sti();
    if (current->entry) current->entry(current->arg);

    /* entry вернулась — мечения задачи к удалению.
       Реально free делаем позже, в idle/reaper'е. */
    cli();
    current->state = TS_DEAD;
    sti();
    /* Принудительный yield — больше сюда не вернёмся. */
    for (;;) sched_yield();
}

/* ---------- Создание задачи ---------- */

task_t* task_create(const char* name, void (*entry)(void*), void* arg) {
    struct task* t = (struct task*)malloc(sizeof(*t));
    if (!t) return NULL;
    t->stack = (char*)malloc(TASK_STACK_SIZE);
    if (!t->stack) { free(t); return NULL; }

    memset(t->stack, 0, TASK_STACK_SIZE);
    t->state = TS_READY;
    t->entry = entry;
    t->arg   = arg;
    t->id    = next_id++;
    t->is_user = 0;
    t->kstack = NULL;
    t->kstack_top = 0;
    t->cr3 = 0;
    t->process = NULL;
    t->saved_user_rsp = 0;
    t->fs_base = 0;
    int i; for (i = 0; i < MAX_NAME-1 && name[i]; i++) t->name[i] = name[i];
    t->name[i] = '\0';

    /* Инициализируем FPU-буфер задачи "чистым" состоянием.
       Простейший путь: скопировать в него текущее состояние, оно
       у нас валидно (fninit вызвана при бутстрапе). */
    memset(t->fxsave, 0, FXSAVE_SIZE);
    __asm__ volatile("fxsave %0" : "=m"(*(u8(*)[512])t->fxsave));

    /* Готовим стек:
       На x86_64 SysV ABI функция ожидает RSP, выровненный так, что
       (RSP mod 16) == 8 на входе — потому что обычно её вызывают через
       CALL, который добавляет 8 байт return address. Мы попадаем в
       task_trampoline через RET, который тоже даёт mod16==0 после pop'а.
       Поэтому конец стека должен быть выровнен на 16 + точка входа
       должна быть на 16-байт границе - 8 байт.

       Раскладка:
         [top, выровнен на 16]
         (никаких лишних байт — мы хотим RSP mod 16 == 0 на момент,
          когда ret попит task_trampoline; внутри trampoline сразу
          push rbp/sub rsp — это OK, главное не вызвать movaps по [rsp+N]
          без своего пролога.)

       На самом деле проще всего: между switch_context.ret и точкой
       где task_trampoline начнёт делать movaps — пройдёт пролог функции,
       который выровняет за нас. НО только если trampoline объявлен
       как обычная C-функция с прологом. Чтобы быть уверенными, кладём
       8 байт padding'а, тогда после ret RSP станет mod16==8, как
       ожидает callee. */
    u64* sp = (u64*)(t->stack + TASK_STACK_SIZE);
    *(--sp) = 0;                          /* выравнивающий 8-байт padding */
    *(--sp) = (u64)task_trampoline;       /* return address для ret */
    *(--sp) = 0;  /* rbx */
    *(--sp) = 0;  /* rbp */
    *(--sp) = 0;  /* r12 */
    *(--sp) = 0;  /* r13 */
    *(--sp) = 0;  /* r14 */
    *(--sp) = 0;  /* r15 */
    t->rsp = sp;

    /* Вставляем в circular list */
    u64 f = save_irq();
    if (!task_list_head) {
        task_list_head = t;
        t->next = t;
    } else {
        t->next = task_list_head->next;
        task_list_head->next = t;
    }
    restore_irq(f);

    return (task_t*)t;
}

/* ---------- Idle ---------- */

static void idle_main(void* arg) {
    (void)arg;
    for (;;) {
        /* hlt разбудит ближайшее прерывание (таймер), и нас тут же
           вытеснят на готовую задачу. */
        __asm__ volatile("hlt");
    }
}

/* ---------- Поиск следующей готовой ---------- */

static struct task* pick_next(struct task* from) {
    struct task* t = from->next;
    /* Проходим круг; если не нашли READY, вернёмся на idle. */
    for (int i = 0; i < 1024; i++) {
        if (t->state == TS_READY) return t;
        t = t->next;
        if (t == from) break;
    }
    /* Дефолт — idle. Idle всегда READY (он крутится в hlt-цикле). */
    return idle_task;
}

/* ---------- Основное переключение ---------- */

#include "gdt.h"

static void do_switch_to(struct task* next) {
    if (next == current) return;

    struct task* prev = current;
    current = next;
    if (prev->state == TS_RUNNING) prev->state = TS_READY;
    next->state = TS_RUNNING;

    /* Если переключаемся на user task — обновим TSS.rsp0, чтобы при
       следующем syscall/прерывании из ring 3 CPU взял правильный
       kernel stack. Для kernel задач TSS.rsp0 не используется
       (мы не меняем уровень привилегий), но обновим всё равно — это
       дёшево и упрощает логику. */
    if (next->kstack_top) {
        tss_set_kernel_stack(next->kstack_top);
        extern void syscall_set_kernel_stack(u64 rsp);
        syscall_set_kernel_stack(next->kstack_top);
    }

    /* Сохраняем percpu.user_rsp_save уходящей задачи и восстанавливаем
       для входящей. percpu.user_rsp_save глобален, но каждая user-задача
       имеет свой user RSP; без этого задача, спящая в kernel (yield в
       sys_wait4/pipe), получала бы чужой user RSP при возврате из syscall. */
    {
        extern u64 syscall_get_user_rsp(void);
        extern void syscall_set_user_rsp(u64);
        prev->saved_user_rsp = syscall_get_user_rsp();
        syscall_set_user_rsp(next->saved_user_rsp);
    }

    /* Восстанавливаем FS-базу (TLS) входящей задачи через IA32_FS_BASE.
       Без этого musl/glibc-программы с thread-local storage падают. */
    if (next->fs_base) {
        u32 lo = (u32)(next->fs_base & 0xFFFFFFFF);
        u32 hi = (u32)(next->fs_base >> 32);
        __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"(lo), "d"(hi));
    }
#ifdef FS_DEBUG
    { extern int printf(const char*,...); printf("<sw %s fs=%lx>", next->name, next->fs_base); }
#endif

    /* Если у task'и свой address space, переключаемся на него */
    if (next->cr3 && next->cr3 != prev->cr3) {
        extern void vmm_switch_space(u64);
        vmm_switch_space(next->cr3);
    }

    switch_context(&prev->rsp, next->rsp, prev->fxsave, next->fxsave);
    /* Сюда возвращаемся, когда нас снова выберут. */
}

void sched_yield(void) {
    if (!sched_enabled || !current) return;
    u64 f = save_irq();
    struct task* nxt = pick_next(current);
    do_switch_to(nxt);
    restore_irq(f);
}

/* Вызывается из IRQ0 (таймера). cli стоит автоматически (interrupt gate),
   поэтому save_irq/restore_irq внутри не нужны. */
void sched_tick(void) {
    pit_tick_inc();
    if (!sched_enabled || !current) return;
    struct task* nxt = pick_next(current);
    if (nxt != current) do_switch_to(nxt);
}

/* ---------- Старт ---------- */

void sched_init(void) {
    /* Превратим текущий поток выполнения (kernel_main) в задачу. */
    struct task* boot = (struct task*)malloc(sizeof(*boot));
    boot->stack = NULL;          /* свой стек, выделенный бутстрапом — не освобождаем */
    boot->state = TS_RUNNING;
    boot->entry = NULL;
    boot->arg   = NULL;
    boot->id    = next_id++;
    boot->is_user = 0;
    boot->kstack = NULL;
    boot->kstack_top = 0;
    boot->cr3 = 0;
    boot->process = NULL;
    boot->saved_user_rsp = 0;
    boot->fs_base = 0;
    memcpy(boot->name, "boot", 5);
    boot->next  = boot;
    memset(boot->fxsave, 0, FXSAVE_SIZE);
    __asm__ volatile("fxsave %0" : "=m"(*(u8(*)[512])boot->fxsave));
    task_list_head = boot;
    current = boot;

    /* Создаём idle. */
    idle_task = (struct task*)task_create("idle", idle_main, NULL);
    /* idle не должен попадать в RR в обычной выдаче — он fallback.
       Уберём его из общего кольца? Нет: тогда мы не сможем переключиться
       на него. Оставим, но pick_next предпочтёт другие READY. */

    sched_enabled = 1;
    printf("[sched] initialized, current='boot' id=%d, idle id=%d\n",
           boot->id, idle_task->id);
}

const char* sched_current_name(void) { return current ? current->name : "?"; }
int         sched_current_id(void)   { return current ? current->id   : 0; }

/* Принудительный exit текущей задачи. После него возврата нет. */
void task_exit(int code) {
    (void)code;
    u64 f = save_irq();
    current->state = TS_DEAD;
    current->cr3 = 0;   /* отказываемся от своего address space */
    restore_irq(f);
    for (;;) sched_yield();
}

u64 sched_current_kernel_stack(void) {
    if (!current) return 0;
    return current->kstack_top;
}

void sched_mark_current_dead(void) {
    if (!current) return;
    current->state = TS_DEAD;
}

/* ---------- Создание user task ----------
 *
 * User задача имеет ДВА стека:
 *   - userspace stack (выделяет caller через mmap-подобный механизм,
 *     передаёт user_rsp в task_create_user)
 *   - kernel stack — отдельный, на нём будут выполняться syscall'ы.
 *     CPU переключится на него по syscall (из TSS.rsp0) или прерыванию.
 *
 * При первом запуске мы должны попасть в ring 3 через iret-trick:
 *   - на kernel stack кладём фиктивный фрейм iret'а:
 *       [SS=USER_DS][RSP=user_rsp][RFLAGS=0x202][CS=USER_CS][RIP=user_entry]
 *   - в верхней части kernel stack — стандартные регистры, как у
 *     обычной задачи, и адрес возврата = user_iret_trampoline (на ASM),
 *     который делает iretq из этого фрейма.
 */

extern void user_iret_trampoline(void);   /* boot/user_enter.asm */

task_t* task_create_user(const char* name, u64 user_entry_rip, u64 user_rsp) {
    struct task* t = (struct task*)malloc(sizeof(*t));
    if (!t) return NULL;

    t->stack  = NULL;     /* не используем — у user'а свой стек */
    t->kstack = (char*)malloc(TASK_STACK_SIZE);
    if (!t->kstack) { free(t); return NULL; }
    memset(t->kstack, 0, TASK_STACK_SIZE);

    t->state = TS_READY;
    t->entry = NULL;
    t->arg   = NULL;
    t->id    = next_id++;
    t->is_user = 1;
    t->cr3 = 0;
    t->process = NULL;
    t->saved_user_rsp = 0;
    t->fs_base = 0;
    int i; for (i = 0; i < MAX_NAME-1 && name[i]; i++) t->name[i] = name[i];
    t->name[i] = '\0';

    memset(t->fxsave, 0, FXSAVE_SIZE);
    __asm__ volatile("fxsave %0" : "=m"(*(u8(*)[512])t->fxsave));

    /* Раскладка kernel stack для user-задачи:
         [top, выровнен на 16]
         iret фрейм:
           SS       = USER_DS                  ← попадает в SS user'а
           RSP      = user_rsp                  ← попадает в RSP user'а
           RFLAGS   = 0x202 (IF=1, bit 1 reserved)
           CS       = USER_CS
           RIP      = user_entry_rip            ← user точка входа
         8 байт padding (выравнивание)
         return address для switch_context.ret = user_iret_trampoline
         6 нулей для callee-saved (rbx,rbp,r12..r15)
    */
    u64* sp = (u64*)(t->kstack + TASK_STACK_SIZE);
    /* iret frame (CPU попит в обратном порядке) */
    *(--sp) = (u64)(0x18 | 3);            /* SS = USER_DS */
    *(--sp) = user_rsp;                    /* RSP */
    *(--sp) = 0x202;                       /* RFLAGS, IF=1 */
    *(--sp) = (u64)(0x20 | 3);             /* CS = USER_CS */
    *(--sp) = user_entry_rip;              /* RIP */
    /* Этот фрейм займут iretq. До него — выравнивание и сохранённые регистры
       так, как ожидает switch_context. Иret-фрейм 40 байт; всего стек выровнен
       на 16, значит iret-фрейм в (top - 40). Top mod 16 == 0, значит
       (top-40) mod 16 == (16-40) mod 16 = 8. То есть в момент iretq
       RSP == 8 mod 16. iret сам по себе на это не реагирует, но как только
       мы вышли в user mode, user-RSP уже мы задали сами.
       Теперь верх kernel stack — это место, на котором мы окажемся
       при возврате из user mode (например, через syscall). Оно
       должно быть mod 16 == 0. До iret-фрейма был top (mod 16 == 0) — это
       и есть будущий RSP0 после iretq, так что TSS.rsp0 = top. */
    t->kstack_top = (u64)(t->kstack + TASK_STACK_SIZE);

    /* А вот для switch_context (первого) нам нужен RSP, который
       указывает на стандартный layout: padding+ret+6 регистров.
       Этот layout мы кладём ПЕРЕД iret-фреймом. */
    *(--sp) = 0;                          /* padding для ABI выравнивания */
    *(--sp) = (u64)user_iret_trampoline;   /* return из switch_context.ret */
    *(--sp) = 0;  /* rbx */
    *(--sp) = 0;  /* rbp */
    *(--sp) = 0;  /* r12 */
    *(--sp) = 0;  /* r13 */
    *(--sp) = 0;  /* r14 */
    *(--sp) = 0;  /* r15 */
    t->rsp = sp;

    /* Вставляем в circular list */
    u64 f = save_irq();
    if (!task_list_head) {
        task_list_head = t;
        t->next = t;
    } else {
        t->next = task_list_head->next;
        task_list_head->next = t;
    }
    restore_irq(f);

    return (task_t*)t;
}

/* ---------- Блокировка задач (для futex и signal-wait) ---------- */

void task_block(void) {
    u64 f = save_irq();
    current->state = TS_BLOCKED;
    restore_irq(f);
    sched_yield();
}

void task_unblock(task_t* t) {
    if (!t) return;
    u64 f = save_irq();
    struct task* st = (struct task*)t;
    if (st->state == TS_BLOCKED) st->state = TS_READY;
    restore_irq(f);
}

task_t* sched_current_task(void) { return (task_t*)current; }

/* clone() — упрощённая: новая user-задача в текущем адресном пространстве. */
task_t* task_clone_user(const char* name, u64 child_entry, u64 child_stack, u64 arg) {
    (void)arg;
    return task_create_user(name, child_entry, child_stack);
}

task_t* sched_find_by_id(int id) {
    if (!task_list_head) return NULL;
    struct task* t = task_list_head;
    do {
        if (t->id == id) return (task_t*)t;
        t = t->next;
    } while (t != task_list_head);
    return NULL;
}

void sched_mark_task_dead(task_t* t) {
    if (!t) return;
    u64 f = save_irq();
    ((struct task*)t)->state = TS_DEAD;
    restore_irq(f);
}

int sched_task_id(task_t* t) {
    if (!t) return -1;
    return ((struct task*)t)->id;
}

/*
 * task_fork_user — создаёт user-task для fork:
 *  - копирует все GP-регистры parent'а через iret frame
 *  - RAX = 0 (child видит fork() returning 0)
 *  - использует fork_iret_trampoline вместо user_iret_trampoline
 *  - cr3 уже должен быть подготовлен caller'ом (vmm_clone)
 */

extern void fork_iret_trampoline(void);

task_t* task_fork_user(const char* name, struct fork_regs* regs) {
    struct task* t = (struct task*)malloc(sizeof(*t));
    if (!t) return NULL;

    t->stack  = NULL;
    t->kstack = (char*)malloc(TASK_STACK_SIZE);
    if (!t->kstack) { free(t); return NULL; }
    memset(t->kstack, 0, TASK_STACK_SIZE);

    t->state = TS_READY;
    t->entry = NULL;
    t->arg   = NULL;
    t->id    = next_id++;
    t->is_user = 1;
    t->cr3 = 0;
    t->process = NULL;
    t->saved_user_rsp = 0;
    t->fs_base = current ? current->fs_base : 0;  /* наследуем TLS-базу от родителя (fork) */
    int i; for (i = 0; i < MAX_NAME-1 && name[i]; i++) t->name[i] = name[i];
    t->name[i] = '\0';

    memset(t->fxsave, 0, FXSAVE_SIZE);
    __asm__ volatile("fxsave %0" : "=m"(*(u8(*)[512])t->fxsave));

    /* Стек: iret frame + GP регистры в обратном порядке (как ожидает trampoline). */
    u64* sp = (u64*)(t->kstack + TASK_STACK_SIZE);

    /* iret frame */
    *(--sp) = (u64)(0x18 | 3);              /* SS */
    *(--sp) = regs->rsp;
    *(--sp) = regs->rflags;
    *(--sp) = (u64)(0x20 | 3);              /* CS */
    *(--sp) = regs->rip;

    /* GP regs (порядок такой, чтобы trampoline pop'ил их корректно):
       первым попит RAX (последним push'ним), значит RAX внизу. */
    *(--sp) = 0;                            /* RAX = 0 для child */
    *(--sp) = regs->rbx;
    *(--sp) = regs->rcx;
    *(--sp) = regs->rdx;
    *(--sp) = regs->rsi;
    *(--sp) = regs->rdi;
    *(--sp) = regs->rbp;
    *(--sp) = regs->r8;
    *(--sp) = regs->r9;
    *(--sp) = regs->r10;
    *(--sp) = regs->r11;
    *(--sp) = regs->r12;
    *(--sp) = regs->r13;
    *(--sp) = regs->r14;
    *(--sp) = regs->r15;

    t->kstack_top = (u64)(t->kstack + TASK_STACK_SIZE);

    *(--sp) = 0;                            /* padding */
    *(--sp) = (u64)fork_iret_trampoline;    /* ret address из switch_context */
    *(--sp) = 0;  /* rbx */
    *(--sp) = 0;  /* rbp */
    *(--sp) = 0;  /* r12 */
    *(--sp) = 0;  /* r13 */
    *(--sp) = 0;  /* r14 */
    *(--sp) = 0;  /* r15 */
    t->rsp = sp;

    u64 f = save_irq();
    if (!task_list_head) {
        task_list_head = t;
        t->next = t;
    } else {
        t->next = task_list_head->next;
        task_list_head->next = t;
    }
    restore_irq(f);
    return (task_t*)t;
}

void task_set_cr3(task_t* t, u64 cr3) {
    if (!t) return;
    ((struct task*)t)->cr3 = cr3;
}

void task_set_process(task_t* t, void* p) {
    if (!t) return;
    ((struct task*)t)->process = p;
}

void* task_get_process(task_t* t) {
    if (!t) return NULL;
    return ((struct task*)t)->process;
}

/* Установить FS-базу текущей задачи (вызывается из sys_arch_prctl). */
void sched_set_fs_base(unsigned long base) {
    if (current) current->fs_base = base;
}
