/*
 * cxx_runtime.cpp — C++ рантайм для freestanding ядра.
 *
 * Что предоставляем:
 *   1. Глобальные конструкторы — вызов по .init_array.
 *   2. new / delete поверх kmalloc/kfree.
 *   3. __cxa_pure_virtual, __cxa_atexit, __dso_handle — Itanium ABI.
 */

#include "types.h"
#include <stdio.h>
#include "heap.h"
#include <string.h>

void* operator new(size_t size)              { return kmalloc(size); }
void* operator new[](size_t size)            { return kmalloc(size); }
void  operator delete(void* p)   noexcept    { kfree(p); }
void  operator delete[](void* p) noexcept    { kfree(p); }
void  operator delete(void* p, size_t)   noexcept { kfree(p); }
void  operator delete[](void* p, size_t) noexcept { kfree(p); }

inline void* operator new(size_t, void* p) noexcept { return p; }

extern "C" void __cxa_pure_virtual() {
    printf("[cxx] pure virtual call!\n");
    for (;;) __asm__ volatile("cli; hlt");
}

extern "C" {
    void* __dso_handle = nullptr;
}

extern "C" int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

typedef void (*ctor_t)(void);
extern "C" ctor_t __init_array_start[];
extern "C" ctor_t __init_array_end[];

extern "C" void cxx_call_global_ctors() {
    for (ctor_t* p = __init_array_start; p < __init_array_end; p++) {
        (*p)();
    }
}
