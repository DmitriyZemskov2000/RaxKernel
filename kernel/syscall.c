/*
 * syscall.c — настройка SYSCALL и dispatcher.
 *
 * MSR'ы:
 *   IA32_EFER (0xC0000080).SCE — включает SYSCALL/SYSRET
 *   IA32_STAR (0xC0000081)     — селекторы
 *   IA32_LSTAR (0xC0000082)    — адрес entry-функции
 *   IA32_FMASK (0xC0000084)    — какие биты RFLAGS очищать
 *   IA32_KERNEL_GS_BASE (0xC0000102) — указатель на per-CPU данные
 *
 * Per-CPU данные у нас:
 *   gs:[0] — слот для сохранения user RSP во время syscall
 *   gs:[8] — kernel RSP (берётся при входе)
 *
 * Эта структура сейчас одна (мы UP), но при SMP их будет N штук,
 * по одной на CPU, и swapgs выберет нужную.
 */

#include "types.h"
#include <stdio.h>
#include <string.h>
#include "syscall.h"
#include "gdt.h"

/* Per-CPU данные. На SMP это будет array per CPU. */
typedef struct {
    u64 user_rsp_save;      /* gs:[0] */
    u64 kernel_rsp;         /* gs:[8] */
    u64 saved_regs_ptr;     /* gs:[16] — указатель на snapshot регистров parent (для fork) */
} percpu_t;

static percpu_t percpu __attribute__((aligned(16)));

extern void syscall_entry(void);

static inline u64 read_msr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static inline void write_msr(u32 msr, u64 v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)));
}

#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_FMASK           0xC0000084
#define MSR_KERNEL_GS_BASE  0xC0000102
#define MSR_GS_BASE         0xC0000101

/* ---------- Системные вызовы ---------- */

/* Forward declarations всех sys_* функций */
extern long sys_write(int fd, const void* buf, size_t n);
extern long sys_read(int fd, void* buf, size_t n);
extern long sys_exit(int code);
extern long sys_getpid(void);
extern long sys_yield(void);
extern long sys_mmap(void* addr, size_t len, int prot, int flags, int fd, long off);
extern long sys_munmap(void* addr, size_t len);
extern long sys_brk(void* addr);
extern long sys_clock_gettime(int clk, void* ts);

/* Таблица: индекс = syscall number, значение = handler.
   Номера совпадают с Linux x86_64. NR_MAX покрывает clock_gettime=228. */
#define NR_MAX 512
typedef long (*syscall_fn_t)(long, long, long, long, long, long);

static syscall_fn_t syscall_table[NR_MAX];

void syscall_install(int nr, syscall_fn_t fn) {
    if (nr >= 0 && nr < NR_MAX) syscall_table[nr] = fn;
}

/*
 * Главный диспетчер. Вызывается из syscall_entry.asm.
 * Сигнатура соответствует тому, как ASM-стаб расставил регистры.
 */
long syscall_dispatch(long num, long a1, long a2, long a3,
                      long a4, long a5, long a6) {
    if (num < 0 || num >= NR_MAX || !syscall_table[num]) {
        return -38;     /* -ENOSYS */
    }
    return syscall_table[num](a1, a2, a3, a4, a5, a6);
}

/* ---------- Установка percpu при переключении задач ----------
 *
 * Каждый раз, когда планировщик переключает задачу, он должен
 * обновить kernel_rsp в percpu — иначе следующий syscall этой
 * задачи прыгнет на чужой kernel stack.
 */
void syscall_set_kernel_stack(u64 rsp) {
    percpu.kernel_rsp = rsp;
    /* TSS.RSP0 тоже обновляем — для прерываний */
    tss_set_kernel_stack(rsp);
}

void syscall_init(void) {
    /* Заглушки */
    for (int i = 0; i < NR_MAX; i++) syscall_table[i] = NULL;

    /* Регистрируем стандартные вызовы. Номера произвольные,
       но совпадают с Linux x86_64 в важных местах. */
    extern long sys_read(int, void*, size_t);
    extern long sys_write(int, const void*, size_t);
    extern long sys_mmap(void*, size_t, int, int, int, long);
    extern long sys_munmap(void*, size_t);
    extern long sys_brk(void*);
    extern long sys_yield(void);
    extern long sys_getpid(void);
    extern long sys_exit(int);
    extern long sys_clock_gettime(int, void*);

    extern long sys_open(const char*, int, int);
    extern long sys_close(int);
    extern long sys_lseek(int, long, int);
    extern long sys_stat_impl(const char*, void*);
    extern long sys_clone(unsigned long, void*, long, long, long, long);
    extern long sys_nanosleep(const void*, void*);
    extern long sys_gettid(void);
    extern long sys_futex(int*, int, int, void*);
    extern long sys_kill(int, int);
    extern long sys_rt_sigaction(int, const void*, void*, size_t);
    extern long sys_rt_sigprocmask(int, const void*, void*, size_t);
    extern long sys_fork(void);
    extern long sys_execve(const char*, char* const[], char* const[]);
    extern long sys_wait4(long, int*, int, void*);
    extern long sys_pipe(int[2]);
    extern long sys_dup(int);
    extern long sys_dup2(int, int);
    extern long sys_getppid(void);
    extern long sys_getuid(void);
    extern long sys_getgid(void);
    extern long sys_access(const char*, int);
    extern long sys_unlink(const char*);
    extern long sys_rmdir(const char*);
    extern long sys_chdir(const char*);
    extern long sys_getcwd(char*, size_t);

    syscall_table[0]   = (syscall_fn_t)sys_read;
    syscall_table[1]   = (syscall_fn_t)sys_write;
    syscall_table[2]   = (syscall_fn_t)sys_open;
    syscall_table[3]   = (syscall_fn_t)sys_close;
    syscall_table[4]   = (syscall_fn_t)sys_stat_impl;
    syscall_table[8]   = (syscall_fn_t)sys_lseek;
    syscall_table[9]   = (syscall_fn_t)sys_mmap;
    syscall_table[11]  = (syscall_fn_t)sys_munmap;
    { extern long sys_mprotect(void*, size_t, int);
      syscall_table[10] = (syscall_fn_t)sys_mprotect; }
    syscall_table[12]  = (syscall_fn_t)sys_brk;
    syscall_table[13]  = (syscall_fn_t)sys_rt_sigaction;
    syscall_table[14]  = (syscall_fn_t)sys_rt_sigprocmask;
    syscall_table[21]  = (syscall_fn_t)sys_access;
    syscall_table[22]  = (syscall_fn_t)sys_pipe;
    syscall_table[24]  = (syscall_fn_t)sys_yield;
    syscall_table[32]  = (syscall_fn_t)sys_dup;
    syscall_table[33]  = (syscall_fn_t)sys_dup2;
    syscall_table[35]  = (syscall_fn_t)sys_nanosleep;
    syscall_table[39]  = (syscall_fn_t)sys_getpid;
    syscall_table[56]  = (syscall_fn_t)sys_clone;
    syscall_table[57]  = (syscall_fn_t)sys_fork;
    syscall_table[59]  = (syscall_fn_t)sys_execve;
    syscall_table[60]  = (syscall_fn_t)sys_exit;
    syscall_table[61]  = (syscall_fn_t)sys_wait4;
    syscall_table[62]  = (syscall_fn_t)sys_kill;
    syscall_table[79]  = (syscall_fn_t)sys_getcwd;
    syscall_table[80]  = (syscall_fn_t)sys_chdir;
    syscall_table[84]  = (syscall_fn_t)sys_rmdir;
    syscall_table[87]  = (syscall_fn_t)sys_unlink;
    syscall_table[102] = (syscall_fn_t)sys_getuid;
    syscall_table[104] = (syscall_fn_t)sys_getgid;
    syscall_table[110] = (syscall_fn_t)sys_getppid;
    syscall_table[186] = (syscall_fn_t)sys_gettid;
    syscall_table[202] = (syscall_fn_t)sys_futex;
    syscall_table[228] = (syscall_fn_t)sys_clock_gettime;
    extern long sys_arch_prctl(int, unsigned long);
    syscall_table[158] = (syscall_fn_t)sys_arch_prctl;
    extern long sys_writev(int, const void*, int);
    extern long sys_ioctl(int, unsigned long, unsigned long);
    extern long sys_set_tid_address(int*);
    extern long sys_exit_group(int);
    syscall_table[20]  = (syscall_fn_t)sys_writev;
    syscall_table[16]  = (syscall_fn_t)sys_ioctl;
    syscall_table[218] = (syscall_fn_t)sys_set_tid_address;
    syscall_table[231] = (syscall_fn_t)sys_exit_group;
    extern long sys_set_robust_list(void*, unsigned long);
    extern long sys_rseq(void*, unsigned int, int, unsigned int);
    syscall_table[273] = (syscall_fn_t)sys_set_robust_list;
    syscall_table[334] = (syscall_fn_t)sys_rseq;
    extern long sys_newfstatat(int, const char*, void*, int);
    extern long sys_fstat(int, void*);
    extern long sys_openat(int, const char*, int, int);
    extern long sys_getrandom(void*, unsigned long, unsigned int);
    extern long sys_prlimit64(int, int, const void*, void*);
    extern long sys_sysinfo(void*);
    extern long sys_gettimeofday(void*, void*);
    extern long sys_readlinkat(int, const char*, char*, unsigned long);
    syscall_table[262] = (syscall_fn_t)sys_newfstatat;
    syscall_table[5]   = (syscall_fn_t)sys_fstat;
    syscall_table[257] = (syscall_fn_t)sys_openat;
    syscall_table[318] = (syscall_fn_t)sys_getrandom;
    syscall_table[302] = (syscall_fn_t)sys_prlimit64;
    syscall_table[99]  = (syscall_fn_t)sys_sysinfo;
    syscall_table[96]  = (syscall_fn_t)sys_gettimeofday;
    syscall_table[267] = (syscall_fn_t)sys_readlinkat;
    extern long sys_pipe2(int[2], int);
    extern long sys_eventfd2(unsigned int, int);
    extern long sys_statfs(const char*, void*);
    extern long sys_tgkill(int, int, int);
    syscall_table[293] = (syscall_fn_t)sys_pipe2;
    syscall_table[290] = (syscall_fn_t)sys_eventfd2;
    syscall_table[137] = (syscall_fn_t)sys_statfs;
    syscall_table[234] = (syscall_fn_t)sys_tgkill;
    extern long sys_prctl(int, unsigned long, unsigned long, unsigned long, unsigned long);
    extern long sys_ppoll(void*, unsigned long, const void*, const void*, unsigned long);
    extern long sys_poll(void*, unsigned long, int);
    syscall_table[157] = (syscall_fn_t)sys_prctl;
    extern long sys_getdents64(int, void*, unsigned long);
    syscall_table[217] = (syscall_fn_t)sys_getdents64;
    syscall_table[78]  = (syscall_fn_t)sys_getdents64;  /* getdents */
    syscall_table[271] = (syscall_fn_t)sys_ppoll;
    syscall_table[7]   = (syscall_fn_t)sys_poll;

    /* Настраиваем MSR'ы */

    /* EFER.SCE = 1 */
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1);

    /* STAR[31:0] = 0 (не используется в 64-bit), STAR[47:32] = kernel CS,
       STAR[63:48] = user_base. См. gdt.h: user_base = 0x10 даст
       user SS = 0x18, user CS = 0x20. */
    u64 star = ((u64)0x08 << 32) | ((u64)0x10 << 48);
    /* Внимание: Intel требует чтобы STAR[63:48]+8 имело DPL=3 и
       было data, STAR[63:48]+16 — code и L=1. Наш GDT именно так
       и устроен (слот 3 = user data, слот 4 = user code). */
    write_msr(MSR_STAR, star);
    write_msr(MSR_LSTAR, (u64)syscall_entry);

    /* FMASK: какие биты RFLAGS очистить при входе. Гасим IF
       (прерывания внутри syscall выключены), DF (направление), TF (trap). */
    write_msr(MSR_FMASK, 0x200 | 0x400 | 0x100);   /* IF | DF | TF */

    /* Per-CPU данные: GSBase = 0 (для userspace),
       KernelGSBase = адрес percpu (swapgs обменяет). */
    percpu.user_rsp_save = 0;
    percpu.kernel_rsp = tss_get_default_kernel_stack();
    /* GS-конвенция (Linux-style):
       - В kernel mode: GS_BASE = percpu — kernel сразу может работать
       - В user mode:   GS_BASE = 0 (или user TLS)
       - KERNEL_GS_BASE хранит "обратное" значение и обменивается swapgs.

       Изначально ставим: GS_BASE = percpu (мы сейчас в kernel),
       KERNEL_GS_BASE = 0 (это пойдёт в user после swapgs). */
    write_msr(MSR_GS_BASE, (u64)&percpu);
    write_msr(MSR_KERNEL_GS_BASE, 0);

    printf("[syscall] enabled, LSTAR=%p, percpu=%p, table %d entries\n",
           (void*)syscall_entry, (void*)&percpu, NR_MAX);
}

/* Доступ к snapshot регистров parent'а для sys_fork. */
u64 syscall_get_saved_regs(void) {
    return percpu.saved_regs_ptr;
}

u64 syscall_get_user_rsp(void) {
    return percpu.user_rsp_save;
}

void syscall_set_user_rsp(u64 rsp) {
    percpu.user_rsp_save = rsp;
}
