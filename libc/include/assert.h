/*
 * assert.h — отладочные утверждения.
 *
 * Реализуем простой assert: при провале — печатает место и
 * вызывает abort(). В release-сборке (-DNDEBUG) разворачивается
 * в no-op.
 */
#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

void __assert_fail(const char* expr, const char* file, int line, const char* func)
    __attribute__((noreturn));

#ifdef NDEBUG
#  define assert(e) ((void)0)
#else
#  define assert(e) \
    ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif

#ifdef __cplusplus
}
#endif

#endif
