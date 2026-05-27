/*
 * pthread.c — реализация POSIX threads через futex + clone.
 *
 * Дизайн mutex (Linux/glibc-style fast mutex):
 *   _state == 0 → unlocked
 *   _state == 1 → locked, contended-free
 *   _state == 2 → locked, есть waiter'ы (нужно wake при unlock)
 *
 * lock:   CAS(0 → 1). Если получилось — done.
 *         Иначе: установить состояние в 2 и futex_wait, пока 2.
 *         После просыпания — попробовать снова.
 * unlock: записать 0. Если предыдущее было 2 — futex_wake(1).
 *
 * Этот паттерн широко используется и теоретически правильный.
 */

#include <pthread.h>
#include <stddef.h>

/* Из user backend'а */
extern long syscall1(long, long);
extern long syscall2(long, long, long);
extern long syscall3(long, long, long, long);
extern long syscall4(long, long, long, long, long);
extern long syscall6(long, long, long, long, long, long, long);

#define SYS_FUTEX       202
#define SYS_CLONE        56
#define SYS_GETTID      186
#define SYS_EXIT         60

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* ---------- atomic helpers (GCC builtins) ---------- */
static inline int atomic_cas(int* p, int old, int new_v) {
    return __atomic_compare_exchange_n(p, &old, new_v, 0,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline int atomic_xchg(int* p, int v) {
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

static inline int atomic_load(int* p) {
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

static inline void atomic_store(int* p, int v) {
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

/* ---------- futex wrappers ---------- */
static int futex_wait(int* uaddr, int val) {
    return (int)syscall4(SYS_FUTEX, (long)uaddr, FUTEX_WAIT, val, 0);
}
static int futex_wake(int* uaddr, int n) {
    return (int)syscall4(SYS_FUTEX, (long)uaddr, FUTEX_WAKE, n, 0);
}

/* ---------- mutex ---------- */

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    (void)a;
    m->_state = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* m) {
    (void)m;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* m) {
    /* Быстрый путь: 0 → 1 */
    if (atomic_cas(&m->_state, 0, 1)) return 0;

    /* Contention. Помечаем 2 (contended), ждём пока станет 0. */
    while (1) {
        int v = atomic_xchg(&m->_state, 2);
        if (v == 0) return 0;
        /* Состояние было 1 или 2. Спим, пока кто-то не разбудит. */
        futex_wait(&m->_state, 2);
    }
}

int pthread_mutex_trylock(pthread_mutex_t* m) {
    return atomic_cas(&m->_state, 0, 1) ? 0 : 16;   /* 16 = EBUSY */
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
    int v = atomic_xchg(&m->_state, 0);
    if (v == 2) {
        futex_wake(&m->_state, 1);
    }
    return 0;
}

/* ---------- cond ----------
 * Семейство примитивов более тонкое. Реализуем простейший:
 *   _seq — глобальный счётчик. wait запоминает текущий seq и ждёт
 *   изменения. signal/broadcast инкрементят seq и wake'ают.
 *
 * Сложности с lost wakeups решены тем, что lock/unlock внешнего
 * mutex'а гарантирует видимость seq.
 */

int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a) {
    (void)a;
    c->_seq = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* c) {
    (void)c;
    return 0;
}

int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    int seq = atomic_load(&c->_seq);
    pthread_mutex_unlock(m);
    futex_wait(&c->_seq, seq);
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t* c) {
    __atomic_fetch_add(&c->_seq, 1, __ATOMIC_SEQ_CST);
    futex_wake(&c->_seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* c) {
    __atomic_fetch_add(&c->_seq, 1, __ATOMIC_SEQ_CST);
    /* INT_MAX wake'ов */
    futex_wake(&c->_seq, 0x7FFFFFFF);
    return 0;
}

/* ---------- pthread_create/exit/self ----------
 *
 * Layout child stack:
 *   [top]
 *   ... 64 KiB ...
 *   start_routine (pointer)        ← thread trampoline снимет
 *   arg                            ← и это
 *   [child_rsp]
 *
 * trampoline (на C): pop start, pop arg, вызвать start(arg), вызвать exit.
 */

extern void __pthread_trampoline(void);

int pthread_create(pthread_t* th, const pthread_attr_t* attr,
                   void* (*start)(void*), void* arg) {
    (void)attr;
    const size_t STACK_SIZE = 64 * 1024;
    char* stack = (char*)malloc(STACK_SIZE);
    if (!stack) return 12;   /* ENOMEM */

    /* Кладём start и arg на верх child stack так, чтобы trampoline их снял.
       Стек растёт вниз, поэтому "верх" = stack + STACK_SIZE. */
    unsigned long* top = (unsigned long*)(stack + STACK_SIZE);
    *(--top) = (unsigned long)arg;      /* верхний qword — arg */
    *(--top) = (unsigned long)start;    /* следующий — start */
    /* Теперь top — child RSP. Trampoline сделает pop rax (start),
       pop rdi (arg). Порядок важен: top[0]=start, top[1]=arg. */

    /* clone(flags=0, child_stack=top, entry=__pthread_trampoline) */
    long tid = syscall6(SYS_CLONE, 0, (long)top,
                        (long)__pthread_trampoline, 0, 0, 0);
    if (tid < 0) { free(stack); return -((int)tid); }
    if (th) *th = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t th, void** retval) {
    (void)retval;
    /* Простейший join: poll по состоянию через несуществующий
       syscall... вместо этого спим, пока tid существует.
       Заведомо упрощено. */
    extern int sched_yield(void);
    for (int i = 0; i < 1000; i++) {
        /* Нет API "is alive" — используем kill(tid, 0) который вернёт ESRCH. */
        long r = syscall2(62, th, 0);  /* SYS_kill */
        if (r != 0) return 0;          /* нет такой задачи — считаем joined */
        sched_yield();
    }
    return 0;
}

void pthread_exit(void* retval) {
    (void)retval;
    syscall1(SYS_EXIT, 0);
    for (;;) {}
}

pthread_t pthread_self(void) {
    return (pthread_t)syscall1(SYS_GETTID, 0);
}
