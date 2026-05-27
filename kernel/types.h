/*
 * types.h — базовые типы для freestanding ядра.
 *
 * Мы компилируем с -ffreestanding и без стандартной библиотеки,
 * поэтому <stdint.h> может быть, а может и не быть доступен в
 * зависимости от тулчейна. Чтобы не зависеть, объявим свои.
 */
#ifndef WEBOS_TYPES_H
#define WEBOS_TYPES_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

typedef signed char         i8;
typedef signed short        i16;
typedef signed int          i32;
typedef signed long long    i64;

/*
 * size_t/ssize_t берём из встроенных типов компилятора —
 * на x86_64 это unsigned long, и совпадение принципиально важно
 * для C++ operator new (компилятор сверяет тип буквально).
 */
typedef __SIZE_TYPE__       size_t;
typedef __PTRDIFF_TYPE__    ssize_t;
typedef __UINTPTR_TYPE__    uintptr_t;

#define NULL ((void*)0)

/* Атрибут "никогда не возвращается" — для panic() и т.п. */
#define NORETURN __attribute__((noreturn))
#define PACKED   __attribute__((packed))
#define ALIGNED(x) __attribute__((aligned(x)))

#endif
