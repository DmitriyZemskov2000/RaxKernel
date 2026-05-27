/*
 * sys_impl.c — реализации системных вызовов.
 *
 * Эти функции вызываются из syscall_dispatch с уже расставленными
 * аргументами. Возвращают long: ≥0 — успех, <0 — отрицательное errno.
 *
 * Важная безопасность: userspace передаёт нам указатели в свой
 * адресный диапазон. Идеально — проверять, что они валидны
 * (copy_from_user / copy_to_user). Пока упрощённо: пользуемся
 * нашим shared address space (kernel mapped в каждом процессе).
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sched.h"
#include "vmm.h"
#include "process.h"
#include "syscall.h"
#include "sched.h"
#include "vmm.h"
#include "pmm.h"

/* Прототип, который ядро использует для вывода */
extern void kputs_raw(const char* s, size_t n);
extern u64  pit_ticks(void);
extern u32  pit_frequency(void);

/* ---------- write/read ---------- */

#include "vfs.h"

long sys_write(int fd, const void* buf, size_t n) {
    if (fd == 1 || fd == 2) {
        /* fast-path: stdout/stderr → консоль напрямую.
           Это и работает без vfs_init_stdio тоже. */
        kputs_raw((const char*)buf, n);
        return (long)n;
    }
    return (long)vfs_write(fd, buf, n);
}

long sys_read(int fd, void* buf, size_t n) {
    if (fd == 0) return 0;   /* stdin: пока EOF (нет клавы) */
    return (long)vfs_read(fd, buf, n);
}

long sys_open(const char* path, int flags, int mode) {
    (void)mode;
    return vfs_open(path, flags);
}

long sys_close(int fd) {
    return vfs_close(fd);
}

long sys_lseek(int fd, long off, int whence) {
    return (long)vfs_lseek(fd, off, whence);
}

long sys_stat_impl(const char* path, void* buf) {
    /* Минимальная struct stat: только size + type. Расширим когда
       появится больше полей. */
    struct {
        u64 size;
        int type;
        int _pad;
    }* out = buf;
    struct vfs_stat st;
    int r = vfs_stat(path, &st);
    if (r < 0) return r;
    out->size = st.size;
    out->type = st.type;
    return 0;
}

/* ---------- mmap / munmap / brk ---------- */

/*
 * Очень упрощённый mmap: только MAP_ANONYMOUS + MAP_PRIVATE,
 * без файлов. fd должен быть -1. addr — hint, мы его игнорируем
 * и выделяем из своего bump-allocator'а user heap.
 */

#define USER_HEAP_BASE  0x50000000ULL          /* 1.25 GiB — за пределами кода+стека */
#define USER_HEAP_LIMIT 0x60000000ULL          /* 1.5 GiB */
static u64 user_heap_top = USER_HEAP_BASE;

long sys_mmap(void* addr, size_t len, int prot, int flags, int fd, long off) {
    (void)addr; (void)prot; (void)flags; (void)off;
    if (fd != -1) return -22;   /* пока только anonymous */
    if (len == 0) return -22;

    /* Округляем len до целых страниц */
    size_t aligned = (len + 4095) & ~4095ULL;

    if (user_heap_top + aligned > USER_HEAP_LIMIT) return -12;   /* -ENOMEM */

    u64 start = user_heap_top;
    /* Маппим страницы в адресное пространство процесса.
       Сейчас у нас один process'ов нет — userspace бежит в том же
       PML4, что и ядро. Так что vmm_map подходит. */
    for (size_t i = 0; i < aligned; i += 4096) {
        void* phys = pmm_alloc_page();
        if (!phys) return -12;
        if (vmm_map(start + i, (u64)phys, VMM_WRITABLE | VMM_USER) != 0) {
            pmm_free_page(phys);
            return -12;
        }
        memset((void*)(start + i), 0, 4096);
    }
    user_heap_top += aligned;
    return (long)start;
}

long sys_munmap(void* addr, size_t len) {
    if (len == 0) return -22;
    size_t aligned = (len + 4095) & ~4095ULL;
    u64 base = (u64)addr & ~4095ULL;
    for (size_t i = 0; i < aligned; i += 4096) {
        u64 phys = vmm_translate(base + i);
        vmm_unmap(base + i);
        if (phys) pmm_free_page((void*)phys);
    }
    return 0;
}

/*
 * brk: примитивная реализация — установить новый конец user-heap.
 * Кладёт страницы по мере роста.
 */

long sys_brk(void* addr) {
    /* brk хранится per-process в process_t. Но самый первый user-процесс
       (init/hello, запущенный через task_create_user) не имеет process_t —
       для него используем глобальный fallback brk. */
    static u64 init_brk = 0;
    const u64 BRK_BASE = 0x60000000ULL;

    task_t* cur = sched_current_task();
    process_t* proc = cur ? (process_t*)task_get_process(cur) : NULL;

    u64* brkp;
    if (proc) {
        if (proc->brk_current == 0) proc->brk_current = BRK_BASE;
        brkp = &proc->brk_current;
    } else {
        if (init_brk == 0) init_brk = BRK_BASE;
        brkp = &init_brk;
    }

    if (addr == NULL) return (long)*brkp;

    u64 want = (u64)addr;
    if (want < BRK_BASE) return (long)*brkp;

    u64 new_top = (want + 4095) & ~4095ULL;
    u64 cur_top = (*brkp + 4095) & ~4095ULL;

    if (new_top > cur_top) {
        for (u64 p = cur_top; p < new_top; p += 4096) {
            void* phys = pmm_alloc_page();
            if (!phys) return -12;
            if (vmm_map(p, (u64)phys, VMM_WRITABLE | VMM_USER) != 0) {
                pmm_free_page(phys);
                return -12;
            }
            memset((void*)p, 0, 4096);
        }
    } else if (new_top < cur_top) {
        for (u64 p = new_top; p < cur_top; p += 4096) {
            u64 phys = vmm_translate(p);
            vmm_unmap(p);
            if (phys) pmm_free_page((void*)phys);
        }
    }
    *brkp = want;
    return (long)*brkp;
}

/* Текущее значение brk вызывающего процесса (для наследования при fork). */
u64 sys_brk_current(void) {
    task_t* cur = sched_current_task();
    process_t* proc = cur ? (process_t*)task_get_process(cur) : NULL;
    if (proc && proc->brk_current) return proc->brk_current;
    /* для init нет process_t — вернём sys_brk(NULL) эквивалент через повторный вызов */
    return (u64)sys_brk(0);
}

/* ---------- остальные ---------- */

long sys_yield(void) {
    sched_yield();
    return 0;
}

long sys_getpid(void) {
    task_t* cur = sched_current_task();
    if (cur) {
        process_t* p = (process_t*)task_get_process(cur);
        if (p) return (long)p->pid;
    }
    return (long)sched_current_id();
}

/* exit пока просто помечает задачу мёртвой. */
long sys_exit(int code) {
    extern void task_exit(int code);
    extern int printf(const char*, ...);
    /* Если task привязана к process_t, помечаем процесс зомби. */
    task_t* cur = sched_current_task();
    if (cur) {
        process_t* p = (process_t*)task_get_process(cur);
        if (p) {
            extern void process_set_zombie(process_t*, int);
            process_set_zombie(p, (code & 0xFF) << 8);
        }
    }
    task_exit(code);
    return 0;
}

long sys_clock_gettime(int clk, void* ts) {
    (void)clk;
    if (!ts) return -22;
    struct {
        long tv_sec;
        long tv_nsec;
    }* out = ts;
    u64 ticks = pit_ticks();
    u32 hz = pit_frequency();
    if (hz == 0) hz = 1;
    out->tv_sec  = (long)(ticks / hz);
    out->tv_nsec = (long)((ticks % hz) * (1000000000ULL / hz));
    return 0;
}

/* ---------- clone() ----------
 * Linux clone() signature:
 *   long clone(unsigned long flags, void* child_stack, ...);
 *
 * Минимальное подмножество: CLONE_VM|CLONE_THREAD. Создаём task
 * в текущем адресном пространстве. У нас все user-задачи и так
 * шарят PML4 — отдельный CR3 для процессов появится позже.
 *
 * child_stack уже должен содержать на верхе entry point ребёнка
 * (стандартное соглашение glibc'а: stack[0] = entry, stack[1] = arg).
 * Мы для упрощения принимаем child_entry как ОТДЕЛЬНЫЙ параметр
 * через flags-overload: a3 = entry, a4 = arg. Это нестандартно,
 * но соответствует тому, как libc pthread_create зовёт sys_clone.
 */
/* Layout snapshot регистров в kernel stack — см. syscall_entry.asm
   (определено здесь, до первого использования в sys_clone) */
typedef struct {
    u64 r15, r14, r13, r12;
    u64 r11;     /* = user RFLAGS */
    u64 r10, r9, r8;
    u64 rbp, rdi, rsi, rdx;
    u64 rcx;     /* = user RIP */
    u64 rbx, rax;
} saved_regs_t;
extern u64 syscall_get_saved_regs(void);

long sys_clone(unsigned long flags, void* child_stack,
               long a3, long a4, long a5, long a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;

    /* musl/glibc fork() реализован через clone() БЕЗ CLONE_VM (0x100).
       Такой clone означает "скопировать адресное пространство" = fork.
       clone С CLONE_VM (pthread_create) — общий address space = поток. */
    #define CLONE_VM_FLAG 0x00000100UL
    if (!(flags & CLONE_VM_FLAG)) {
        extern long sys_fork(void);
        return sys_fork();
    }

    /* Поток: как fork, НО общий cr3 (та же память) и СВОЙ стек.
       После clone дочерний поток продолжает с rax=0 на той же RIP;
       libc-трамплин сам снимет start_routine/arg со своего стека и
       вызовет их. Мы НЕ прыгаем на entry сами. */
    saved_regs_t* regs = (saved_regs_t*)syscall_get_saved_regs();
    if (!regs) return -12;

    extern u64 vmm_current_cr3(void);
    u64 cur_cr3 = vmm_current_cr3();

    struct fork_regs fr;
    fr.rax = 0;                  /* поток видит clone() = 0 */
    fr.rbx = regs->rbx; fr.rcx = regs->rcx; fr.rdx = regs->rdx;
    fr.rsi = regs->rsi; fr.rdi = regs->rdi; fr.rbp = regs->rbp;
    fr.r8  = regs->r8;  fr.r9  = regs->r9;  fr.r10 = regs->r10;
    fr.r11 = regs->r11; fr.r12 = regs->r12; fr.r13 = regs->r13;
    fr.r14 = regs->r14; fr.r15 = regs->r15;
    fr.rip = regs->rcx;          /* возврат на ту же точку после syscall */
    fr.rflags = regs->r11;
    fr.rsp = (u64)child_stack;   /* СВОЙ стек потока (от libc) */

    extern task_t* task_fork_user(const char*, struct fork_regs*);
    task_t* t = task_fork_user("thread", &fr);
    if (!t) return -12;

    /* Общая память: тот же cr3 что у родителя (НЕ клонируем). */
    extern void task_set_cr3(task_t*, u64);
    extern void task_set_process(task_t*, void*);
    extern void* task_get_process(task_t*);
    task_set_cr3(t, cur_cr3);
    task_t* cur = sched_current_task();
    if (cur) task_set_process(t, task_get_process(cur));

    extern int sched_task_id(task_t* t);
    return (long)sched_task_id(t);
}

long sys_nanosleep(const void* req, void* rem) {
    (void)rem;
    if (!req) return -22;
    const struct {
        long tv_sec;
        long tv_nsec;
    }* r = req;
    long ms = r->tv_sec * 1000 + r->tv_nsec / 1000000;
    if (ms < 0) ms = 0;
    extern void pit_sleep_ms(u32 ms);
    pit_sleep_ms((u32)ms);
    return 0;
}

long sys_gettid(void) {
    /* В нашей UP-модели tid == pid. */
    return (long)sched_current_id();
}

/* ---------- fork ---------- */

#include "process.h"

/* Layout snapshot регистров в kernel stack — см. syscall_entry.asm */

long sys_fork(void) {
    saved_regs_t* regs = (saved_regs_t*)syscall_get_saved_regs();
    if (!regs) return -12;

    /* Clone address space (новый PML4 с скопированными user-страницами) */
    u64 new_cr3 = vmm_clone_address_space();
    if (!new_cr3) return -12;

    /* Создаём process_t для child. parent_pid должен быть process-pid
       родителя (а не task-id) — иначе sys_wait4 (ищущий по process-pid)
       не найдёт child. */
    extern process_t* process_create(int);
    extern void* task_get_process(task_t*);
    task_t* cur_task = sched_current_task();
    process_t* parent_proc = cur_task ? (process_t*)task_get_process(cur_task) : NULL;
    int parent_pid_for_child = parent_proc ? parent_proc->pid : sched_current_id();
    process_t* child = process_create(parent_pid_for_child);
    if (!child) return -12;
    child->cr3 = new_cr3;
    /* Наследуем brk родителя — иначе child думает что heap пуст, хотя
       vmm_clone уже скопировал страницы heap, и malloc в child ломается. */
    extern u64 sys_brk_current(void);
    child->brk_current = sys_brk_current();

    /* Конструируем struct fork_regs для task_fork_user (определена в sched.h) */
    struct fork_regs fr;

    fr.rax = 0;                          /* child видит fork() = 0 */
    fr.rbx = regs->rbx;
    fr.rcx = regs->rcx;
    fr.rdx = regs->rdx;
    fr.rsi = regs->rsi;
    fr.rdi = regs->rdi;
    fr.rbp = regs->rbp;
    fr.r8  = regs->r8;
    fr.r9  = regs->r9;
    fr.r10 = regs->r10;
    fr.r11 = regs->r11;
    fr.r12 = regs->r12;
    fr.r13 = regs->r13;
    fr.r14 = regs->r14;
    fr.r15 = regs->r15;
    fr.rip = regs->rcx;                  /* user RIP сохранён в RCX */
    fr.rflags = regs->r11;
    /* User RSP — из percpu.user_rsp_save */
    extern u64 syscall_get_user_rsp(void);
    fr.rsp = syscall_get_user_rsp();
#ifdef SYSCALL_DEBUG
    {
        extern int printf(const char*, ...);
        printf("[fork rip=%lx rsp=%lx rcx=%lx]", fr.rip, fr.rsp, fr.rcx);
    }
#endif

    task_t* child_task = task_fork_user("child", &fr);
    if (!child_task) {
        process_free(child);
        return -12;
    }
    task_set_cr3(child_task, new_cr3);
    task_set_process(child_task, child);

    /* Parent тоже должен помнить свой текущий cr3 + process. */
    extern u64 vmm_current_cr3(void);
    extern int printf(const char*, ...);
    task_t* parent_task = sched_current_task();
    if (parent_task) {
        task_set_cr3(parent_task, vmm_current_cr3());
        if (!task_get_process(parent_task)) {
            /* Parent ещё не имеет process_t (это первый fork от init).
               Создадим для него. */
            process_t* p = process_create(0);   /* parent_pid = 0 (init) */
            if (p) {
                p->cr3 = vmm_current_cr3();
                task_set_process(parent_task, p);
                child->parent_pid = p->pid;   /* пересчитаем parent_pid */
            }
        }
    }

    return (long)child->pid;
}

/* ---------- execve ----------
 *
 * Заменяет код+данные текущего процесса новой программой.
 * Не возвращается при успехе — переключает RIP на новую entry.
 */
long sys_execve(const char* path, char* const argv[], char* const envp[]) {
    if (!path) return -14;   /* -EFAULT */

    extern void* kmalloc(size_t);
    extern void  kfree(void*);
    /* Прочитаем ELF из VFS */
    extern int vfs_open(const char*, int);
    extern int vfs_close(int);
    extern ssize_t vfs_read(int, void*, size_t);
    extern int vfs_stat(const char*, struct vfs_stat*);

    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0) return -2;   /* -ENOENT */
    if (st.size == 0) return -7;

    int fd = vfs_open(path, 0);
    if (fd < 0) return -2;

    /* Скопируем argv/envp в kernel-buffer ДО загрузки сегментов
       (сегменты могут перезаписать страницы, где лежат старые argv). */
    char* argv_copy[16];
    char* envp_copy[16];
    int argc = 0, envc = 0;
    if (argv) {
        while (argc < 15 && argv[argc]) {
            size_t l = strlen(argv[argc]) + 1;
            argv_copy[argc] = (char*)kmalloc(l);
            if (!argv_copy[argc]) break;
            memcpy(argv_copy[argc], argv[argc], l);
            argc++;
        }
    }
    argv_copy[argc] = NULL;
    if (envp) {
        while (envc < 15 && envp[envc]) {
            size_t l = strlen(envp[envc]) + 1;
            envp_copy[envc] = (char*)kmalloc(l);
            if (!envp_copy[envc]) break;
            memcpy(envp_copy[envc], envp[envc], l);
            envc++;
        }
    }
    envp_copy[envc] = NULL;

    /* Загружаем ELF ПОТОКОВО прямо из fd — без чтения всего файла в
       kernel heap. Это позволяет грузить большие бинари (cc1 207 МБ). */
    extern int elf_load_fd(int, u64*);
    u64 entry = 0;
    int lr = elf_load_fd(fd, &entry);
    vfs_close(fd);
    if (lr != 0) {
        for (int i = 0; i < argc; i++) kfree(argv_copy[i]);
        for (int i = 0; i < envc; i++) kfree(envp_copy[i]);
        return -8;   /* -ENOEXEC */
    }

    /* Создаём новый user stack — 32 KB */
    extern int vmm_map(u64, u64, u64);
    extern void* pmm_alloc_page(void);
    const u64 NEW_STACK_TOP = 0x40800000ULL;
    const u64 NEW_STACK_SIZE = 1024 * 1024;   /* 1 MiB — tcc-линковщику нужен глубокий стек */
    for (u64 a = NEW_STACK_TOP - NEW_STACK_SIZE; a < NEW_STACK_TOP; a += 4096) {
        void* p = pmm_alloc_page();
        if (!p) break;
        vmm_map(a, (u64)p, 0x2 | 0x4);   /* WRITABLE|USER */
        memset((void*)a, 0, 4096);
    }

    /* Строим SysV-стек с argv/envp */
    char* sp = (char*)NEW_STACK_TOP;
    char* argv_strs[16];
    char* envp_strs[16];
    for (int i = argc - 1; i >= 0; i--) {
        size_t l = strlen(argv_copy[i]) + 1;
        sp -= l;
        memcpy(sp, argv_copy[i], l);
        argv_strs[i] = sp;
    }
    for (int i = envc - 1; i >= 0; i--) {
        size_t l = strlen(envp_copy[i]) + 1;
        sp -= l;
        memcpy(sp, envp_copy[i], l);
        envp_strs[i] = sp;
    }
    sp = (char*)((u64)sp & ~15ULL);

    /* 16 байт для AT_RANDOM (glibc использует для stack canary _dl_random).
       Без этого glibc разыменует _dl_random==0 → page fault. */
    sp -= 16;
    {
        /* Простые псевдослучайные байты (для AT_RANDOM достаточно). */
        u64 seed = 0x9e3779b97f4a7c15ULL ^ (u64)sp;
        for (int i = 0; i < 16; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            sp[i] = (char)(seed >> 33);
        }
    }
    char* at_random_ptr = sp;
    sp = (char*)((u64)sp & ~15ULL);

    /* Считаем слоты: argc + argv[argc] + NULL + envp[envc] + NULL
       + auxv (AT_PAGESZ, AT_RANDOM, AT_NULL = 3 пары = 6 слов). */
    int auxv_words = 6;
    int slots = 1 + argc + 1 + envc + 1 + auxv_words;
    if (slots & 1) sp -= 8;

    /* auxv (кладём первым, т.к. стек растёт вниз — окажется выше всех) */
    sp -= 8; *(u64*)sp = 0;                    /* AT_NULL значение */
    sp -= 8; *(u64*)sp = 0;                    /* AT_NULL тип */
    sp -= 8; *(u64*)sp = (u64)at_random_ptr;   /* AT_RANDOM значение */
    sp -= 8; *(u64*)sp = 25;                   /* AT_RANDOM тип */
    sp -= 8; *(u64*)sp = 4096;                 /* AT_PAGESZ значение */
    sp -= 8; *(u64*)sp = 6;                    /* AT_PAGESZ тип */
    sp -= 8; *(u64*)sp = 0;          /* envp NULL */
    for (int i = envc - 1; i >= 0; i--) { sp -= 8; *(u64*)sp = (u64)envp_strs[i]; }
    sp -= 8; *(u64*)sp = 0;          /* argv NULL */
    for (int i = argc - 1; i >= 0; i--) { sp -= 8; *(u64*)sp = (u64)argv_strs[i]; }
    sp -= 8; *(u64*)sp = (u64)argc;

    /* Освобождаем kernel-копии */
    for (int i = 0; i < argc; i++) kfree(argv_copy[i]);
    for (int i = 0; i < envc; i++) kfree(envp_copy[i]);

    /* Сбрасываем TLB — мы перемаппили страницы на те же vaddr,
       старые трансляции надо инвалидировать. Перезагрузка CR3 = flush. */
    extern u64 vmm_current_cr3(void);
    extern void vmm_switch_space(u64);
    vmm_switch_space(vmm_current_cr3());

    /* Обновим percpu.kernel_rsp на свежий top kstack. */
    extern u64 sched_current_kernel_stack(void);
    extern void syscall_set_kernel_stack(u64);
    syscall_set_kernel_stack(sched_current_kernel_stack());

    /* Переход в новую программу. Не возвращается. */
    extern void execve_to_user(u64 rip, u64 rsp) __attribute__((noreturn));
    execve_to_user(entry, (u64)sp);
    return 0;   /* недостижимо */
}

long sys_wait4(long pid, int* status, int options, void* rusage) {
    (void)rusage;
    extern process_t* process_find_zombie_child(int);
    extern int process_has_children(int);
    extern void process_free(process_t*);

    /* Узнаём свой process_t pid */
    int parent_pid = 1;
    task_t* cur = sched_current_task();
    if (cur) {
        process_t* p = (process_t*)task_get_process(cur);
        if (p) parent_pid = p->pid;
    }

    if (!process_has_children(parent_pid)) return -10;  /* -ECHILD */

    while (1) {
        process_t* z = process_find_zombie_child(parent_pid);
        if (z) {
            if (pid != -1 && pid != z->pid) {
                /* Скипаем, попробуем yield и поищем заново */
            } else {
                if (status) *status = z->exit_status;
                int child_pid = z->pid;
                process_free(z);
                return child_pid;
            }
        }
        if (options & 1) return 0;       /* WNOHANG */
        extern long sys_yield(void);
        sys_yield();
    }
}

long sys_pipe(int fds[2]) {
    if (!fds) return -14;
    extern int pipe_create(vnode_t**, vnode_t**);
    vnode_t *r, *w;
    if (pipe_create(&r, &w) != 0) return -23;   /* -ENFILE */
    int rfd = vfs_install_fd(r);
    if (rfd < 0) return rfd;
    int wfd = vfs_install_fd(w);
    if (wfd < 0) return wfd;
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

long sys_dup(int fd) {
    return (long)vfs_dup(fd);
}

long sys_dup2(int oldfd, int newfd) {
    return (long)vfs_dup2(oldfd, newfd);
}

long sys_getppid(void) {
    task_t* cur = sched_current_task();
    if (cur) {
        process_t* p = (process_t*)task_get_process(cur);
        if (p) return (long)p->parent_pid;
    }
    return 0;
}

long sys_getuid(void) { return 0; }   /* root */
long sys_getgid(void) { return 0; }

long sys_access(const char* path, int mode) {
    (void)mode;
    struct vfs_stat st;
    int r = vfs_stat(path, &st);
    return (r < 0) ? -2 : 0;     /* -ENOENT или 0 */
}

long sys_unlink(const char* path) { (void)path; return -30; }  /* -EROFS */
long sys_rmdir(const char* path)  { (void)path; return -30; }

static char cwd_buf[256] = "/";

long sys_chdir(const char* path) {
    if (!path) return -22;
    size_t n = 0;
    while (path[n] && n < sizeof(cwd_buf) - 1) { cwd_buf[n] = path[n]; n++; }
    cwd_buf[n] = '\0';
    return 0;
}

long sys_getcwd(char* buf, size_t size) {
    if (!buf || size == 0) return -22;
    size_t n = 0;
    while (cwd_buf[n] && n < size - 1) { buf[n] = cwd_buf[n]; n++; }
    buf[n] = '\0';
    return (long)n;
}

/* mprotect — у нас все user-страницы уже writable+user, защита прав
   пока не реализована на уровне PTE per-region. Возвращаем успех,
   чтобы код, который зовёт mprotect (например JIT в tcc), работал. */
long sys_mprotect(void* addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return 0;
}

/* arch_prctl(code, addr) — управление сегментными базами (TLS).
   musl и glibc используют ARCH_SET_FS для thread-local storage.
   ARCH_SET_FS=0x1002, ARCH_GET_FS=0x1003, ARCH_SET_GS=0x1001, ARCH_GET_GS=0x1004 */
long sys_arch_prctl(int code, unsigned long addr) {
    /* MSR: IA32_FS_BASE=0xC0000100, IA32_GS_BASE=0xC0000101 */
    switch (code) {
    case 0x1002: { /* ARCH_SET_FS */
        u32 lo = (u32)(addr & 0xFFFFFFFF);
        u32 hi = (u32)(addr >> 32);
        __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"(lo), "d"(hi));
        /* Сохраняем в текущей задаче, чтобы восстанавливать при переключении */
        extern void sched_set_fs_base(unsigned long);
        sched_set_fs_base(addr);
        return 0;
    }
    case 0x1003: { /* ARCH_GET_FS */
        u32 lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
        unsigned long v = ((unsigned long)hi << 32) | lo;
        if (addr) *(unsigned long*)addr = v;
        return 0;
    }
    case 0x1001: { /* ARCH_SET_GS — обычно не трогаем (kernel использует GS) */
        return -22; /* -EINVAL: не поддерживаем, чтобы не сломать swapgs */
    }
    default:
        return -22; /* -EINVAL */
    }
}

/* writev — вектор записей (musl printf использует его). */
struct _iovec { void* iov_base; unsigned long iov_len; };
long sys_writev(int fd, const struct _iovec* iov, int iovcnt) {
    extern long sys_write(int, const void*, unsigned long);
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        long r = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return (total > 0) ? total : r;
        total += r;
        if ((unsigned long)r < iov[i].iov_len) break;
    }
    return total;
}

/* ioctl — заглушка. musl зовёт TIOCGWINSZ/TCGETS для isatty. Возвращаем
   -ENOTTY, чтобы isatty() корректно вернул 0 (не терминал). */
long sys_ioctl(int fd, unsigned long req, unsigned long arg) {
    (void)fd; (void)req; (void)arg;
    return -25; /* -ENOTTY */
}

/* set_tid_address — заглушка, возвращаем фиктивный tid. */
long sys_set_tid_address(int* tidptr) {
    (void)tidptr;
    return 1;
}

/* exit_group — как exit (у нас один поток на процесс). */
long sys_exit_group(int code) {
    extern long sys_exit(int);
    return sys_exit(code);
}

/* set_robust_list (273) — futex robust list, glibc зовёт при старте потока.
   Заглушка: просто успех. */
long sys_set_robust_list(void* head, unsigned long len) {
    (void)head; (void)len;
    return 0;
}

/* rseq (334) — restartable sequences, glibc зовёт при старте.
   Заглушка: -ENOSYS отключает фичу в glibc (она это терпит). */
long sys_rseq(void* rseq, unsigned int len, int flags, unsigned int sig) {
    (void)rseq; (void)len; (void)flags; (void)sig;
    return -38;  /* glibc корректно работает без rseq */
}

/* ===== Syscalls для glibc-программ (GCC cc1) ===== */

#define AT_FDCWD_VAL (-100)

/* Полная Linux struct stat (x86_64). glibc разбирает все поля. */
struct linux_stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned long st_nlink;
    unsigned int  st_mode;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned int  __pad0;
    unsigned long st_rdev;
    long          st_size;
    long          st_blksize;
    long          st_blocks;
    unsigned long st_atime; unsigned long st_atime_nsec;
    unsigned long st_mtime; unsigned long st_mtime_nsec;
    unsigned long st_ctime; unsigned long st_ctime_nsec;
    long          __unused[3];
};

static void fill_linux_stat(struct linux_stat* ls, struct vfs_stat* st) {
    extern void* memset(void*, int, unsigned long);
    memset(ls, 0, sizeof(*ls));
    ls->st_dev = 1;
    ls->st_ino = 1;
    ls->st_nlink = 1;
    /* type: 1=file(VNODE_FILE), 2=dir(VNODE_DIR) */
    ls->st_mode = (st->type == 2) ? (0040000 | 0755) : (0100000 | 0644);
    ls->st_size = (long)st->size;
    ls->st_blksize = 4096;
    ls->st_blocks = (st->size + 511) / 512;
}

/* newfstatat(262) — stat по dirfd+path. cc1 активно использует. */
long sys_newfstatat(int dirfd, const char* path, void* statbuf, int flags) {
    (void)flags;
    struct vfs_stat st;
    /* Поддерживаем абсолютные пути и AT_FDCWD. */
    if (dirfd != AT_FDCWD_VAL && path && path[0] != '/') return -2;
    int r = vfs_stat(path, &st);
    if (r < 0) return r;
    fill_linux_stat((struct linux_stat*)statbuf, &st);
    return 0;
}

/* fstat(5) — stat по fd. Для нашего vfs упрощённо. */
long sys_fstat(int fd, void* statbuf) {
    extern void* memset(void*, int, unsigned long);
    extern long vfs_fsize(int);
    extern int  vfs_ftype(int);
    struct linux_stat* ls = (struct linux_stat*)statbuf;
    memset(ls, 0, sizeof(*ls));
    ls->st_dev = 1; ls->st_ino = 1; ls->st_nlink = 1;
    long sz = vfs_fsize(fd);
    int ty = vfs_ftype(fd);
    if (sz >= 0) {
        ls->st_mode = (ty == 2) ? (0040000|0755) : (0100000|0644);
        ls->st_size = sz;
    } else {
        /* stdin/stdout/stderr или неизвестный fd — character device */
        ls->st_mode = 0020000 | 0644;  /* S_IFCHR */
    }
    ls->st_blksize = 4096;
    ls->st_blocks = (ls->st_size + 511) / 512;
    return 0;
}

/* openat(257) — open относительно dirfd. Поддерживаем AT_FDCWD + абс пути. */
long sys_openat(int dirfd, const char* path, int flags, int mode) {
    (void)mode;
    if (dirfd != AT_FDCWD_VAL && path && path[0] != '/') return -2;
    return vfs_open(path, flags);
}

/* getrandom(318) — заполняет буфер псевдослучайными байтами. */
long sys_getrandom(void* buf, unsigned long len, unsigned int flags) {
    (void)flags;
    static u64 seed = 0x123456789abcdef0ULL;
    unsigned char* p = (unsigned char*)buf;
    for (unsigned long i = 0; i < len; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = (unsigned char)(seed >> 33);
    }
    return (long)len;
}

/* prlimit64(302) — лимиты ресурсов. Возвращаем "бесконечность". */
long sys_prlimit64(int pid, int resource, const void* new_limit, void* old_limit) {
    (void)pid; (void)resource; (void)new_limit;
    if (old_limit) {
        unsigned long* ol = (unsigned long*)old_limit;
        ol[0] = ~0UL;  /* rlim_cur = RLIM_INFINITY */
        ol[1] = ~0UL;  /* rlim_max */
    }
    return 0;
}

/* sysinfo(99) — информация о системе. Заполняем минимум. */
long sys_sysinfo(void* info) {
    extern void* memset(void*, int, unsigned long);
    /* struct sysinfo большой; обнулим, заполним totalram. */
    memset(info, 0, 112);
    unsigned long* u = (unsigned long*)info;
    u[0] = 100;           /* uptime */
    /* поля loads[3] идут как unsigned long... упрощённо оставим totalram */
    return 0;
}

/* gettimeofday(96) — время. Заглушка с фиксированным значением. */
long sys_gettimeofday(void* tv, void* tz) {
    (void)tz;
    if (tv) {
        long* t = (long*)tv;
        t[0] = 1700000000;  /* tv_sec (фикс) */
        t[1] = 0;           /* tv_usec */
    }
    return 0;
}

/* readlinkat(267) — чтение символьной ссылки. У нас их нет → -EINVAL. */
long sys_readlinkat(int dirfd, const char* path, char* buf, unsigned long bufsiz) {
    (void)dirfd; (void)path; (void)buf; (void)bufsiz;
    return -22;  /* -EINVAL: не симлинк */
}

/* ===== Syscalls для GTK/GLib ===== */

/* pipe2 (293) — pipe с флагами (O_CLOEXEC/O_NONBLOCK). Игнорируем флаги. */
long sys_pipe2(int fds[2], int flags) {
    extern long sys_pipe(int[2]);
    (void)flags;
    return sys_pipe(fds);
}

/* eventfd2 (290) — счётчик-событие для main loop. Минимальная реализация:
   возвращаем fd на /dev/null-подобный объект. GLib использует для
   пробуждения main loop. Дадим рабочий fd через pipe. */
long sys_eventfd2(unsigned int initval, int flags) {
    (void)initval; (void)flags;
    /* Создаём pipe и возвращаем его read-конец как eventfd-заглушку.
       Этого хватает чтобы poll не падал. */
    int fds[2];
    extern long sys_pipe(int[2]);
    if (sys_pipe(fds) < 0) return -24;
    /* закрываем write-конец нам не нужен отдельно — оставим оба,
       вернём read-конец. */
    return fds[0];
}

/* statfs (137) — информация о ФС. Заглушка с разумными значениями. */
long sys_statfs(const char* path, void* buf) {
    extern void* memset(void*, int, unsigned long);
    (void)path;
    /* struct statfs ~120 байт; обнулим и заполним базовое. */
    memset(buf, 0, 120);
    unsigned long* u = (unsigned long*)buf;
    u[0] = 0xEF53;        /* f_type = EXT2 magic */
    u[1] = 4096;          /* f_bsize */
    u[2] = 1024*1024;     /* f_blocks */
    u[3] = 512*1024;      /* f_bfree */
    u[4] = 512*1024;      /* f_bavail */
    return 0;
}

/* tgkill (234) — послать сигнал потоку. Для self-abort: завершаем процесс. */
long sys_tgkill(int tgid, int tid, int sig) {
    (void)tgid; (void)tid;
    extern long sys_exit(int);
    sys_exit(128 + sig);
    return 0;
}

/* prctl (157) — управление процессом (имя потока и пр.). Заглушка-успех. */
long sys_prctl(int option, unsigned long a2, unsigned long a3,
               unsigned long a4, unsigned long a5) {
    (void)option; (void)a2; (void)a3; (void)a4; (void)a5;
    return 0;
}

/* poll/ppoll — опрос готовности fd. Структура pollfd. */
struct linux_pollfd { int fd; short events; short revents; };

/* ppoll (271) — poll с таймаутом и сигнальной маской.
   Минимальная реализация: помечаем готовыми (POLLIN) валидные fd,
   чтобы GTK main loop не блокировался навечно. */
long sys_ppoll(struct linux_pollfd* fds, unsigned long nfds,
               const void* tmo, const void* sigmask, unsigned long ss) {
    (void)tmo; (void)sigmask; (void)ss;
    extern long vfs_fsize(int);
    int ready = 0;
    for (unsigned long i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        /* Если fd валиден и ждут чтения — помечаем готовым (есть данные
           или EOF, GTK обработает). Это poll-as-ready семантика —
           заставляет main loop крутиться, не вешаясь. */
        if (fds[i].fd >= 0 && (fds[i].events & 0x001 /*POLLIN*/)) {
            fds[i].revents = 0x001;
            ready++;
        }
    }
    /* небольшая пауза чтобы не жечь CPU на busy-loop */
    extern void pit_sleep_ms(u32);
    if (!ready) pit_sleep_ms(10);
    return ready;
}

/* poll (7) — то же без сигнальной маски. */
long sys_poll(struct linux_pollfd* fds, unsigned long nfds, int timeout) {
    (void)timeout;
    return sys_ppoll(fds, nfds, 0, 0, 0);
}

/* ---- getdents64 (217): чтение записей каталога ---- */
struct linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

long sys_getdents64(int fd, void* buf, unsigned long count) {
    extern vnode_t* vfs_vnode_of_fd(int);
    extern long vfs_get_dir_pos(int);
    extern void vfs_set_dir_pos(int, long);
    extern int  vfs_readdir(vnode_t*, size_t, char*, size_t);
    vnode_t* v = vfs_vnode_of_fd(fd);
    if (!v) return -9;   /* EBADF */
    long pos = vfs_get_dir_pos(fd);
    unsigned long off = 0;
    char namebuf[256];
    while (off + sizeof(struct linux_dirent64) + 8 < count) {
        int r = vfs_readdir(v, (size_t)pos, namebuf, sizeof(namebuf));
        if (r != 0) break;   /* кончились */
        unsigned namelen = 0;
        while (namebuf[namelen] && namelen < 255) namelen++;
        unsigned reclen = (sizeof(struct linux_dirent64) + namelen + 1 + 7) & ~7u;
        if (off + reclen > count) break;
        struct linux_dirent64* d = (struct linux_dirent64*)((char*)buf + off);
        d->d_ino = (unsigned long long)(pos + 1);
        d->d_off = (long long)(off + reclen);
        d->d_reclen = (unsigned short)reclen;
        d->d_type = 0;   /* DT_UNKNOWN — glibc сделает stat если нужно */
        for (unsigned i=0;i<namelen;i++) d->d_name[i]=namebuf[i];
        d->d_name[namelen]=0;
        off += reclen;
        pos++;
    }
    vfs_set_dir_pos(fd, pos);
    return (long)off;
}
