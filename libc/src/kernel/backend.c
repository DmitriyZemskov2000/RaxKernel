/*
 * libc_backend_kernel.c — реализация backend-хуков libc для ядра.
 *
 * Этот файл собирается ТОЛЬКО в libk.a (ядерный билд libc).
 * В userspace варианте libc.a его заменяет libc_backend_user.c.
 *
 * Хуки:
 *   __libc_malloc_impl / __libc_realloc_impl / __libc_free_impl
 *       — heap-аллокатор ядра
 *   __libc_write_impl   — пишет в VGA + serial (kputs_raw)
 *   __libc_exit_impl    — panic + halt
 */

#include <stddef.h>

/* Эти символы предоставляет ядро (kernel/heap.c) */
extern void* kmalloc(size_t);
extern void* krealloc(void*, size_t);
extern void  kfree(void*);

/* И ядерный kputs (kernel/kputs.c). У него такая же сигнатура,
   как у будущего __libc_write_impl, — просто алиасим. */
extern void kputs_raw(const char* s, size_t n);

void* __libc_malloc_impl(size_t n)            { return kmalloc(n); }
void* __libc_realloc_impl(void* p, size_t n)  { return krealloc(p, n); }
void  __libc_free_impl(void* p)               { kfree(p); }

void  __libc_write_impl(const char* s, size_t n) { kputs_raw(s, n); }

__attribute__((noreturn))
void  __libc_exit_impl(int status) {
    (void)status;
    /* В ядре нет места куда выйти — halt'имся. */
    for (;;) __asm__ volatile("cli; hlt");
}
