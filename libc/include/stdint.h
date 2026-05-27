/*
 * stdint.h — целочисленные типы фиксированной ширины.
 * Прямые типы (не GCC-builtin __INT8_TYPE__), чтобы работало и с tcc.
 * На x86_64 LP64: int=32, long=64, long long=64.
 */
#ifndef _STDINT_H
#define _STDINT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef signed char         int8_t;
typedef short               int16_t;
typedef int                 int32_t;
typedef long                int64_t;

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long       uint64_t;

typedef long                intptr_t;
typedef unsigned long       uintptr_t;

typedef long                intmax_t;
typedef unsigned long       uintmax_t;

#define INT8_MIN    (-128)
#define INT16_MIN   (-32768)
#define INT32_MIN   (-2147483647 - 1)
#define INT64_MIN   (-9223372036854775807LL - 1)

#define INT8_MAX    127
#define INT16_MAX   32767
#define INT32_MAX   2147483647
#define INT64_MAX   9223372036854775807LL

#define UINT8_MAX   255U
#define UINT16_MAX  65535U
#define UINT32_MAX  4294967295U
#define UINT64_MAX  18446744073709551615ULL

#define SIZE_MAX    UINT64_MAX

#ifdef __cplusplus
}
#endif

#endif
