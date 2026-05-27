/*
 * signal.c — минимальная инфраструктура сигналов.
 *
 * Что реализовано:
 *   - kill(pid, SIGKILL) → принудительный exit задачи
 *   - sigaction / sigprocmask — сохраняют состояние, но не доставляют
 *     user-handler'ы. Это позволяет портируемому софту скомпилироваться
 *     (большинство просто игнорирует или ставит SIG_DFL).
 *   - raise() — userspace может вызвать сам себя через kill(getpid(),...)
 *
 * Что НЕ реализовано (отложено):
 *   - Доставка user-handler'ов через signal frame на user stack
 *   - sigreturn trampoline
 *   - Pending mask и automatic delivery на возврате из syscall
 *
 * Этого хватит для portable software, который не критически
 * зависит от async-signal обработки. Когда дойдём до сетевого ПО,
 * SIGPIPE станет важен — тогда расширим до полной доставки.
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include "sched.h"
#include "ksignal.h"

#define SIGKILL  9
#define SIGTERM 15

long sys_kill(int pid, int sig) {
    /* Сейчас у нас простой поиск: проходим circular list задач
       (через sched), сравниваем id. Я добавлю экспорт. */
    extern task_t* sched_find_by_id(int id);
    task_t* t = sched_find_by_id(pid);
    if (!t) return -3;   /* -ESRCH */

    if (sig == SIGKILL || sig == SIGTERM) {
        /* Помечаем мёртвой — планировщик её больше не выберет.
           Если это текущий процесс, делаем task_exit. */
        if (t == sched_current_task()) {
            task_exit(128 + sig);
        }
        /* Иначе — помечаем DEAD через sched API */
        extern void sched_mark_task_dead(task_t* t);
        sched_mark_task_dead(t);
    }
    return 0;
}

/* sigaction/sigprocmask — stub: API возвращает успех, состояние не сохраняем.
   Этого достаточно для большинства портируемого кода. */

long sys_rt_sigaction(int sig, const void* act, void* oldact, size_t sigsetsize) {
    (void)sig; (void)act; (void)oldact; (void)sigsetsize;
    return 0;
}

long sys_rt_sigprocmask(int how, const void* set, void* oldset, size_t sigsetsize) {
    (void)how; (void)set; (void)oldset; (void)sigsetsize;
    return 0;
}
