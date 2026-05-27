/*
 * dlfcn.h — POSIX dynamic loading.
 *
 * Полная реализация требует динамического линкера (ld.so), который
 * мы построим в отдельной итерации после networking. Пока — stub'ы,
 * чтобы портируемый код, использующий dlopen в optional-режиме,
 * скомпилировался и просто получил NULL.
 */
#ifndef _DLFCN_H
#define _DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY    0x1
#define RTLD_NOW     0x2
#define RTLD_GLOBAL  0x100
#define RTLD_LOCAL   0
#define RTLD_DEFAULT ((void*)0)
#define RTLD_NEXT    ((void*)-1)

void*       dlopen(const char* filename, int flag);
const char* dlerror(void);
void*       dlsym(void* handle, const char* symbol);
int         dlclose(void* handle);

#ifdef __cplusplus
}
#endif

#endif
