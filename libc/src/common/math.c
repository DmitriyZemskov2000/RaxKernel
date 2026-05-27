/*
 * math.c — реализация <math.h>.
 *
 * Дизайн: используем union с uint64_t для битовых трюков —
 * это позволяет работать с double без подключения SSE/x87.
 * Полиномиальные приближения для трансцендентных функций
 * дают приемлемую точность для bring-up'а и отладочной печати.
 *
 * Когда добавим FPU save/restore — заменим внутренности на
 * нативный код.
 */

#include <math.h>
#include <stdint.h>

/* ---------- Битовое представление double ---------- */

typedef union { double d; uint64_t u; } d64;

#define EXP_BIAS    1023
#define EXP_MASK    0x7FF0000000000000ULL
#define FRAC_MASK   0x000FFFFFFFFFFFFFULL
#define SIGN_MASK   0x8000000000000000ULL

static inline int exp_of(uint64_t u) { return (int)((u >> 52) & 0x7FF) - EXP_BIAS; }

/* ---------- Базовые ---------- */

double fabs(double x) {
    d64 v; v.d = x;
    v.u &= ~SIGN_MASK;
    return v.d;
}

float fabsf(float x) {
    union { float f; uint32_t u; } v; v.f = x;
    v.u &= 0x7FFFFFFF;
    return v.f;
}

double copysign(double x, double y) {
    d64 vx, vy; vx.d = x; vy.d = y;
    vx.u = (vx.u & ~SIGN_MASK) | (vy.u & SIGN_MASK);
    return vx.d;
}

int isnan(double x) {
    d64 v; v.d = x;
    return ((v.u & EXP_MASK) == EXP_MASK) && (v.u & FRAC_MASK);
}

int isinf(double x) {
    d64 v; v.d = x;
    return ((v.u & EXP_MASK) == EXP_MASK) && !(v.u & FRAC_MASK);
}

int isfinite(double x) {
    d64 v; v.d = x;
    return (v.u & EXP_MASK) != EXP_MASK;
}

int signbit(double x) {
    d64 v; v.d = x;
    return (v.u >> 63) & 1;
}

int fpclassify(double x) {
    d64 v; v.d = x;
    uint64_t e = v.u & EXP_MASK;
    uint64_t f = v.u & FRAC_MASK;
    if (e == EXP_MASK) return f ? FP_NAN : FP_INFINITE;
    if (e == 0) return f ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

/* ---------- Округление (точное, через манипуляции с экспонентой) ---------- */

double trunc(double x) {
    d64 v; v.d = x;
    int e = exp_of(v.u);
    if (e < 0) {                                /* |x| < 1 — обрезаем до ±0 */
        v.u &= SIGN_MASK;
        return v.d;
    }
    if (e >= 52) return x;                      /* нет дробной части */
    uint64_t mask = FRAC_MASK >> e;
    v.u &= ~mask;
    return v.d;
}

double floor(double x) {
    double t = trunc(x);
    if (t == x || x >= 0) return t;
    return t - 1.0;
}

double ceil(double x) {
    double t = trunc(x);
    if (t == x || x <= 0) return t;
    return t + 1.0;
}

double round(double x) {
    /* Half away from zero, как требует C99. */
    if (x >= 0) return floor(x + 0.5);
    return ceil(x - 0.5);
}

double fmod(double x, double y) {
    if (y == 0.0 || isnan(x) || isnan(y) || isinf(x)) return NAN;
    if (isinf(y)) return x;
    double q = trunc(x / y);
    return x - q * y;
}

/* ---------- sqrt (Newton-Raphson) ---------- */
/*
 * Стартовое приближение берём через "магию" с экспонентой:
 * для x = 1.f * 2^e имеем sqrt(x) ≈ 1.f' * 2^(e/2).
 * Сдвигом экспоненты получаем неплохой первоначальный guess,
 * затем 4 итерации Newton'а дают полные 53 бита точности.
 */
double sqrt(double x) {
    if (x < 0) return NAN;
    if (x == 0) return 0;
    if (isinf(x)) return x;

    d64 v; v.d = x;
    /* (e + bias) / 2 ≈ e/2 + bias/2; чтобы вернуть bias обратно — добавим bias/2 */
    int e = (int)((v.u >> 52) & 0x7FF);
    int new_e = ((e - EXP_BIAS) >> 1) + EXP_BIAS;
    v.u = ((uint64_t)new_e << 52) | (v.u & FRAC_MASK);
    double y = v.d;

    /* 5 итераций — с запасом для double */
    for (int i = 0; i < 5; i++) {
        y = 0.5 * (y + x / y);
    }
    return y;
}

/* ---------- exp / log ---------- */
/*
 * exp(x) = 2^(x/ln2)
 * Разложим x = k*ln2 + r, |r| <= ln2/2. Тогда
 *   exp(x) = 2^k * exp(r).
 * exp(r) аппроксимируем многочленом Тейлора 10-й степени.
 * 2^k — манипуляция с экспонентой double.
 */
double exp(double x) {
    if (isnan(x)) return x;
    if (x >  709.0) return INFINITY;            /* overflow */
    if (x < -745.0) return 0.0;                  /* underflow */

    const double LN2 = 0.6931471805599453;
    double k_d = x / LN2;
    double k_r = (k_d >= 0) ? (double)(int64_t)(k_d + 0.5)
                            : (double)(int64_t)(k_d - 0.5);
    double r = x - k_r * LN2;

    /* exp(r) — Taylor */
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 20; i++) {
        term *= r / i;
        sum += term;
        if (fabs(term) < 1e-17 * fabs(sum)) break;
    }

    /* 2^k */
    int64_t k = (int64_t)k_r;
    d64 v; v.d = 1.0;
    int64_t new_exp = (int64_t)((v.u >> 52) & 0x7FF) + k;
    if (new_exp >= 0x7FF) return INFINITY;
    if (new_exp <= 0)     return 0.0;
    v.u = ((uint64_t)new_exp << 52) | (v.u & FRAC_MASK);
    return sum * v.d;
}

double log(double x) {
    if (isnan(x) || x < 0) return NAN;
    if (x == 0) return -INFINITY;
    if (isinf(x)) return x;

    /* log(x) = log(m * 2^e) = e*ln2 + log(m), где m ∈ [1, 2). */
    d64 v; v.d = x;
    int e = exp_of(v.u);
    v.u = (v.u & FRAC_MASK) | ((uint64_t)EXP_BIAS << 52);   /* m */
    double m = v.d;

    /* log(m) для m ∈ [1, 2): подставим u = (m-1)/(m+1), |u| < 1/3
       log(m) = 2 * (u + u^3/3 + u^5/5 + ...). Сходится быстро. */
    double u = (m - 1.0) / (m + 1.0);
    double u2 = u * u;
    double sum = 0.0, term = u;
    for (int i = 0; i < 30; i++) {
        sum += term / (2 * i + 1);
        term *= u2;
        if (fabs(term) < 1e-17) break;
    }
    return 2.0 * sum + e * 0.6931471805599453;
}

double exp2(double x) { return exp(x * M_LN2); }
double log2(double x) { return log(x) / M_LN2; }
double log10(double x){ return log(x) / M_LN10; }

double pow(double x, double y) {
    if (y == 0) return 1.0;
    if (x == 0) return 0.0;
    if (x < 0) {
        /* y должно быть целым, иначе результат — комплекс */
        double yt = trunc(y);
        if (yt != y) return NAN;
        double r = exp(y * log(-x));
        int64_t yi = (int64_t)yt;
        return (yi & 1) ? -r : r;
    }
    return exp(y * log(x));
}

/* ---------- sin / cos / tan ----------
 *
 * Reduce x в [-π/4, π/4] квадрантами, затем Taylor 12-й степени.
 * Точность ~1e-15 в reduced диапазоне.
 */
static void reduce_pi2(double x, double* r, int* q) {
    /* q = round(x / (π/2)); r = x - q*(π/2) */
    const double PI_2 = 1.57079632679489661923;
    double n = (x >= 0) ? (double)(int64_t)(x / PI_2 + 0.5)
                        : (double)(int64_t)(x / PI_2 - 0.5);
    *r = x - n * PI_2;
    *q = (int)((int64_t)n & 3);
}

static double sin_kernel(double r) {
    double r2 = r * r;
    double term = r, sum = r;
    for (int i = 1; i < 10; i++) {
        term *= -r2 / ((2*i) * (2*i + 1));
        sum += term;
        if (fabs(term) < 1e-17 * fabs(sum)) break;
    }
    return sum;
}

static double cos_kernel(double r) {
    double r2 = r * r;
    double term = 1, sum = 1;
    for (int i = 1; i < 10; i++) {
        term *= -r2 / ((2*i - 1) * (2*i));
        sum += term;
        if (fabs(term) < 1e-17 * fabs(sum)) break;
    }
    return sum;
}

double sin(double x) {
    if (isnan(x) || isinf(x)) return NAN;
    double r; int q;
    reduce_pi2(x, &r, &q);
    switch (q & 3) {
        case 0: return  sin_kernel(r);
        case 1: return  cos_kernel(r);
        case 2: return -sin_kernel(r);
        case 3: return -cos_kernel(r);
    }
    return 0;
}

double cos(double x) {
    if (isnan(x) || isinf(x)) return NAN;
    double r; int q;
    reduce_pi2(x, &r, &q);
    switch (q & 3) {
        case 0: return  cos_kernel(r);
        case 1: return -sin_kernel(r);
        case 2: return -cos_kernel(r);
        case 3: return  sin_kernel(r);
    }
    return 0;
}

double tan(double x) {
    double c = cos(x);
    if (c == 0) return copysign(INFINITY, sin(x));
    return sin(x) / c;
}

/* ---------- atan / asin / acos ----------
 * atan через сводку к |t| <= 1 и atan(t) = sum (-1)^k t^(2k+1)/(2k+1).
 * Для скорости сходимости при |t| близком к 1 используем
 * atan(t) = atan(1) + atan((t-1)/(t+1)).
 */
double atan(double x) {
    if (isnan(x)) return x;
    double sign = (x < 0) ? -1.0 : 1.0;
    x = fabs(x);
    int high_branch = 0;
    if (x > 1.0) { x = 1.0 / x; high_branch = 1; }

    int piover4 = 0;
    if (x > 0.4142135623730951) {   /* tan(π/8) */
        x = (x - 1.0) / (x + 1.0);
        piover4 = 1;
    }
    double x2 = x * x;
    double term = x, sum = x;
    for (int i = 1; i < 50; i++) {
        term *= -x2;
        double add = term / (2 * i + 1);
        sum += add;
        if (fabs(add) < 1e-17) break;
    }
    if (piover4) sum += M_PI_4;
    if (high_branch) sum = M_PI_2 - sum;
    return sign * sum;
}

double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0 && y >= 0) return atan(y / x) + M_PI;
    if (x < 0 && y <  0) return atan(y / x) - M_PI;
    if (x == 0 && y > 0) return  M_PI_2;
    if (x == 0 && y < 0) return -M_PI_2;
    return 0;   /* (0,0) */
}

double asin(double x) {
    if (x < -1 || x > 1) return NAN;
    if (x == 1)  return M_PI_2;
    if (x == -1) return -M_PI_2;
    return atan(x / sqrt(1 - x * x));
}

double acos(double x) {
    if (x < -1 || x > 1) return NAN;
    return M_PI_2 - asin(x);
}

/* ldexp: x * 2^exp */
double ldexp(double x, int exp) {
    double r = x;
    if (exp > 0) { for (int i = 0; i < exp; i++) r *= 2.0; }
    else         { for (int i = 0; i < -exp; i++) r *= 0.5; }
    return r;
}
float ldexpf(float x, int exp) { return (float)ldexp(x, exp); }
long double ldexpl(long double x, int exp) {
    long double r = x;
    if (exp > 0) { for (int i = 0; i < exp; i++) r *= 2.0L; }
    else         { for (int i = 0; i < -exp; i++) r *= 0.5L; }
    return r;
}

/* ---- float-обёртки для Nuklear/stb_truetype ---- */
float sqrtf(float x){ return (float)sqrt((double)x); }
float cosf(float x){ return (float)cos((double)x); }
float powf(float x, float y){ return (float)pow((double)x,(double)y); }
float fmodf(float x, float y){ return (float)fmod((double)x,(double)y); }
float acosf(float x){ return (float)acos((double)x); }
float floorf(float x){ return (float)floor((double)x); }
float ceilf(float x){ return (float)ceil((double)x); }
