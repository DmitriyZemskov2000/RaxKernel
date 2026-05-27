/*
 * libc_backend_user.c — backend libc для userspace.
 *
 * __libc_write_impl  → SYS_WRITE(1, buf, n)
 * __libc_exit_impl   → SYS_EXIT(status)
 * malloc/free/realloc — простейший аллокатор поверх SYS_BRK.
 *
 * Этот malloc намеренно простой — мы хотим только bring-up. Когда
 * проект дозреет, заменим на dlmalloc или такой же first-fit как
 * в ядре, но через brk вместо pmm_alloc_page.
 */

#include <stddef.h>
#include <stdint.h>

/* Прототипы ASM-обёрток */
extern long syscall1(long num, long a1);
extern long syscall3(long num, long a1, long a2, long a3);

#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_LSEEK   8
#define SYS_BRK     12
#define SYS_UNLINK  87
#define SYS_EXIT    60

/* ---------- write & exit ---------- */

void __libc_write_impl(const char* s, size_t n) {
    syscall3(SYS_WRITE, 1, (long)s, (long)n);
}

__attribute__((noreturn))
void __libc_exit_impl(int status) {
    syscall1(SYS_EXIT, status);
    for (;;) {}     /* compiler: noreturn */
}

/* ---------- FILE-backend хуки ---------- */

int __libc_open_impl(const char* path, int flags) {
    return (int)syscall3(SYS_OPEN, (long)path, flags, 0);
}

long __libc_read_impl(int fd, void* buf, size_t n) {
    return syscall3(SYS_READ, fd, (long)buf, (long)n);
}

long __libc_write_fd_impl(int fd, const void* buf, size_t n) {
    return syscall3(SYS_WRITE, fd, (long)buf, (long)n);
}

int __libc_close_impl(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

long __libc_lseek_impl(int fd, long off, int whence) {
    return syscall3(SYS_LSEEK, fd, off, whence);
}

int __libc_unlink_impl(const char* path) {
    return (int)syscall1(SYS_UNLINK, (long)path);
}

/* ---------- Bump-allocator поверх brk ----------
 *
 * Самая простая стратегия: храним указатель текущего конца heap'а,
 * двигаем его brk()'ом по мере роста. free — no-op (память не
 * возвращаем). Это OK для коротких userspace программ; для долгоживущих
 * нужен настоящий free-list — добавим в следующих итерациях.
 */

static char*  heap_start = NULL;
static char*  heap_end   = NULL;
static char*  heap_curr  = NULL;

static void heap_init_lazy(void) {
    if (heap_start) return;
    long base = syscall1(SYS_BRK, 0);
    heap_start = (char*)base;
    heap_end   = (char*)base;
    heap_curr  = (char*)base;
}

static int heap_extend(size_t needed) {
    /* Кратно 4 KiB */
    size_t grow = (needed + 4095) & ~4095UL;
    long  newbk = (long)((uintptr_t)heap_end + grow);
    long  got   = syscall1(SYS_BRK, newbk);
    if (got != newbk) return 0;
    heap_end = (char*)got;
    return 1;
}

void* __libc_malloc_impl(size_t size) {
    if (size == 0) return NULL;
    heap_init_lazy();

    /* Выравнивание payload'а на 16 байт */
    size_t aligned = (size + 15) & ~15UL;

    /* Записываем размер ПЕРЕД блоком — чтобы realloc мог его прочитать. */
    size_t total = aligned + 16;
    if ((size_t)(heap_end - heap_curr) < total) {
        if (!heap_extend(total)) return NULL;
    }
    char* hdr = heap_curr;
    *(size_t*)hdr = aligned;
    heap_curr += total;
    return hdr + 16;
}

void __libc_free_impl(void* p) {
    /* bump-allocator — память не возвращаем. */
    (void)p;
}

void* __libc_realloc_impl(void* p, size_t newsize) {
    if (!p) return __libc_malloc_impl(newsize);
    if (newsize == 0) { __libc_free_impl(p); return NULL; }

    size_t old = *(size_t*)((char*)p - 16);
    if (newsize <= old) return p;

    void* np = __libc_malloc_impl(newsize);
    if (!np) return NULL;
    /* memcpy */
    char* d = (char*)np; char* s = (char*)p;
    for (size_t i = 0; i < old; i++) d[i] = s[i];
    return np;
}

/* time.c в common/ зависит от kernel'ных pit_* функций. В userspace
   их нет — даём стабы через syscall clock_gettime. Реальное время
   получают через gettimeofday/clock_gettime напрямую. */
unsigned long long pit_ticks(void) {
    struct { long sec; long nsec; } ts = {0,0};
    syscall2(228, 1 /*CLOCK_MONOTONIC*/, (long)&ts);
    return (unsigned long long)ts.sec * 100ULL + ts.nsec / 10000000ULL;
}
unsigned pit_frequency(void) { return 100; }
void pit_sleep_ms(unsigned ms) {
    struct { long sec; long nsec; } ts;
    ts.sec = ms / 1000;
    ts.nsec = (long)(ms % 1000) * 1000000L;
    struct { long sec; long nsec; } rem;
    syscall2(35 /*nanosleep*/, (long)&ts, (long)&rem);
}
