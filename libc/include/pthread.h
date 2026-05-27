/*
 * pthread.h — POSIX threads.
 *
 * Минимальная реализация поверх futex и clone. Подмножество достаточное
 * для портирования библиотек уровня zlib, libpng, freetype.
 */
#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long pthread_t;

typedef struct {
    int   _state;       /* 0 = unlocked, 1 = locked, 2 = contended */
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }

typedef struct {
    int   _seq;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER { 0 }

typedef int pthread_attr_t;        /* пока пустой */
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;

int  pthread_create(pthread_t* th, const pthread_attr_t* attr,
                    void* (*start)(void*), void* arg);
int  pthread_join(pthread_t th, void** retval);
void pthread_exit(void* retval) __attribute__((noreturn));
pthread_t pthread_self(void);

int  pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a);
int  pthread_mutex_destroy(pthread_mutex_t* m);
int  pthread_mutex_lock(pthread_mutex_t* m);
int  pthread_mutex_trylock(pthread_mutex_t* m);
int  pthread_mutex_unlock(pthread_mutex_t* m);

int  pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a);
int  pthread_cond_destroy(pthread_cond_t* c);
int  pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m);
int  pthread_cond_signal(pthread_cond_t* c);
int  pthread_cond_broadcast(pthread_cond_t* c);

#ifdef __cplusplus
}
#endif

#endif
