/*
 * math.h — математические функции.
 *
 * Внимание: эта реализация — INTEGER-ONLY (bit hacks через union с uint64_t),
 * чтобы работать в kernel-mode с отключённым SSE. Это даёт correct rounding
 * на 1-2 ULP для большинства диапазонов, но не быстрее аппаратной FPU.
 *
 * Когда мы добавим сохранение FPU state в context switch (итерация 4),
 * перепишем эти функции на нативный double — будет быстрее.
 */
#ifndef _MATH_H
#define _MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Константы */
#define M_E         2.7182818284590452354
#define M_LOG2E     1.4426950408889634074
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440

#define HUGE_VAL    (1e300 * 1e300)
#define INFINITY    __builtin_inff()
#define NAN         __builtin_nanf("")

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

/* Базовые операции на битовом уровне (всегда корректны) */
double fabs(double x);
float  fabsf(float x);
double copysign(double x, double y);

/* Классификация */
int    fpclassify(double x);
int    isnan(double x);
int    isinf(double x);
int    isfinite(double x);
int    signbit(double x);

/* Округление */
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);

/* Корни и степени */
double sqrt(double x);
double pow(double x, double y);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);

/* Тригонометрия */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double ldexp(double x, int exp);
float  ldexpf(float x, int exp);
long double ldexpl(long double x, int exp);


float sqrtf(float x);
float cosf(float x);
float powf(float x, float y);
float fmodf(float x, float y);
float acosf(float x);
float floorf(float x);
float ceilf(float x);

#ifdef __cplusplus
}
#endif

#endif
