/*
 * stddef.h — стандартные типы.
 *
 * Минимум по C11: size_t, ptrdiff_t, NULL, offsetof, wchar_t.
 * Не определяем max_align_t — он редко нужен и тянет за собой
 * платформенные сюрпризы.
 */
#ifndef _STDDEF_H
#define _STDDEF_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long       size_t;
typedef long                ptrdiff_t;

#ifndef __cplusplus
typedef int                 wchar_t;
#endif

#ifndef NULL
#  ifdef __cplusplus
#    define NULL nullptr
#  else
#    define NULL ((void*)0)
#  endif
#endif

#define offsetof(t, m) __builtin_offsetof(t, m)

#ifdef __cplusplus
}
#endif

#endif
