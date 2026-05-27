/*
 * futex.c — fast userspace mutex backend.
 *
 * Идея futex: пользователю даётся 32-битное слово в его памяти.
 * Быстрый путь (lock/unlock) — atomic compare-and-swap на этом слове,
 * БЕЗ syscall'а. Медленный путь (contention) — futex_wait/futex_wake
 * через syscall:
 *
 *   futex(uaddr, FUTEX_WAIT, val, timeout)
 *     если *uaddr == val: блокировать задачу, добавив в wait-queue.
 *     иначе: вернуть EAGAIN (значение успело измениться).
 *
 *   futex(uaddr, FUTEX_WAKE, n)
 *     разбудить до n задач, ждущих на uaddr.
 *
 * Wait-queue хранится через хеш-таблицу по адресу. Простейшая
 * реализация — global list waiting tasks. При нашем масштабе хватит.
 *
 * Это базис всего pthread: mutex, condvar, semaphore — поверх futex.
 */

#include "types.h"
#include <stdio.h>
#include "sched.h"
#include "futex.h"

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* Простая очередь ожидающих. В реальной ОС было бы hash by uaddr. */
typedef struct fwaiter {
    u64 uaddr;             /* виртуальный адрес слова в адр.простр. процесса */
    task_t* task;          /* спящая задача */
    struct fwaiter* next;
} fwaiter_t;

static fwaiter_t* waiters = NULL;

extern void task_block(void);                /* реализовано в sched.c */
extern void task_unblock(task_t* t);
extern void* kmalloc(size_t);
extern void  kfree(void*);

long sys_futex(int* uaddr, int op, int val, void* timeout) {
    (void)timeout;
    if (!uaddr) return -22;
    /* glibc добавляет флаги к op: FUTEX_PRIVATE_FLAG(128),
       FUTEX_CLOCK_REALTIME(256). Маскируем — оставляем базовую операцию. */
    int base_op = op & 0x7F;   /* убираем флаги (биты 7+) */
    switch (base_op) {
    case FUTEX_WAIT: {
        /* Сравниваем атомарно — но мы UP и без прерываний в syscall'е,
           обычное сравнение ОК. */
        if (*(volatile int*)uaddr != val) return -11;   /* EAGAIN */

        fwaiter_t* w = (fwaiter_t*)kmalloc(sizeof(*w));
        if (!w) return -12;
        w->uaddr = (u64)uaddr;
        w->task  = sched_current_task();
        w->next  = waiters;
        waiters = w;

        task_block();   /* возвращается, когда нас разбудит wake */
        return 0;
    }
    case FUTEX_WAKE: {
        int woken = 0;
        fwaiter_t** pp = &waiters;
        while (*pp && woken < val) {
            fwaiter_t* w = *pp;
            if (w->uaddr == (u64)uaddr) {
                *pp = w->next;
                task_unblock(w->task);
                kfree(w);
                woken++;
            } else {
                pp = &w->next;
            }
        }
        return woken;
    }
    case 9:  /* FUTEX_WAIT_BITSET — как WAIT, игнорируем bitset */
        if (*(volatile int*)uaddr != val) return -11;
        {
            fwaiter_t* w = (fwaiter_t*)kmalloc(sizeof(*w));
            if (!w) return -12;
            w->uaddr = (u64)uaddr; w->task = sched_current_task();
            w->next = waiters; waiters = w;
            task_block();
        }
        return 0;
    case 10: /* FUTEX_WAKE_BITSET — как WAKE */
    case 3:  /* FUTEX_REQUEUE — упрощённо как WAKE */
    {
        int woken = 0;
        fwaiter_t** pp = &waiters;
        while (*pp && woken < val) {
            fwaiter_t* w = *pp;
            if (w->uaddr == (u64)uaddr) {
                *pp = w->next; task_unblock(w->task); kfree(w); woken++;
            } else pp = &w->next;
        }
        return woken;
    }
    default:
        return 0;     /* неизвестные futex-операции — мягкий успех */
    }
}
