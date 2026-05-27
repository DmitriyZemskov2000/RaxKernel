/*
 * dlfcn.c — stub реализация dlopen/dlsym.
 *
 * Возвращаем NULL/ошибку — без настоящего ld.so мы не можем загружать
 * shared object'ы. Главное — что код компилируется.
 */

#include <dlfcn.h>
#include <stddef.h>

static const char* g_err = "dynamic loading not supported";

void* dlopen(const char* filename, int flag) {
    (void)filename; (void)flag;
    return NULL;
}

const char* dlerror(void) {
    const char* e = g_err;
    g_err = NULL;
    return e;
}

void* dlsym(void* handle, const char* symbol) {
    (void)handle; (void)symbol;
    return NULL;
}

int dlclose(void* handle) {
    (void)handle;
    return 0;
}
