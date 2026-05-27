/*
 * stdlib.c — реализация <stdlib.h>.
 *
 * Аллокатор: используем абстрактный backend через слабые символы.
 * В ядре kernel/libc_backend_kernel.c реализует их через kmalloc/kfree
 * из heap.c. В userspace user/libc_backend_user.c реализует через
 * brk()/mmap() сисколлы.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Backend hooks — должны быть определены вне libc/common. */
extern void* __libc_malloc_impl(size_t);
extern void* __libc_realloc_impl(void*, size_t);
extern void  __libc_free_impl(void*);

void* malloc(size_t size) { return __libc_malloc_impl(size); }

void* calloc(size_t nmemb, size_t size) {
    if (size && nmemb > (size_t)-1 / size) return NULL;
    size_t total = nmemb * size;
    void* p = __libc_malloc_impl(total);
    if (p) memset(p, 0, total);
    return p;
}

void* realloc(void* p, size_t size) { return __libc_realloc_impl(p, size); }
void  free(void* p)                 { __libc_free_impl(p); }

/* ---------- Числовые преобразования ---------- */

static int is_digit(int c)        { return c >= '0' && c <= '9'; }
static int is_space(int c)        { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int is_alpha_lo(int c)     { return c >= 'a' && c <= 'z'; }
static int is_alpha_hi(int c)     { return c >= 'A' && c <= 'Z'; }
static int hex_digit(int c) {
    if (is_digit(c))    return c - '0';
    if (is_alpha_lo(c)) return c - 'a' + 10;
    if (is_alpha_hi(c)) return c - 'A' + 10;
    return -1;
}

long strtol(const char* s, char** endp, int base) {
    const char* p = s;
    while (is_space(*p)) p++;

    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }

    /* Автоопределение базы для 0/0x */
    if (base == 0) {
        if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (*p == '0') { base = 8; p++; }
        else base = 10;
    } else if (base == 16 && *p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    long result = 0;
    int any = 0;
    for (;;) {
        int d = hex_digit(*p);
        if (d < 0 || d >= base) break;
        result = result * base + d;
        p++;
        any = 1;
    }

    if (endp) *endp = (char*)(any ? p : s);
    return neg ? -result : result;
}

unsigned long strtoul(const char* s, char** endp, int base) {
    /* Упрощённо — берём ту же логику без знака. */
    const char* p = s;
    while (is_space(*p)) p++;
    if (*p == '+') p++;

    int saw_zero_prefix = 0;
    if (base == 0) {
        if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (*p == '0') { base = 8; p++; saw_zero_prefix = 1; }
        else base = 10;
    } else if (base == 16 && *p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    } else if (base == 8 && *p == '0') {
        p++; saw_zero_prefix = 1;
    }

    unsigned long result = 0;
    int any = saw_zero_prefix;   /* одиночный "0" уже валиден */
    for (;;) {
        int d = hex_digit(*p);
        if (d < 0 || d >= base) break;
        result = result * (unsigned long)base + (unsigned long)d;
        p++; any = 1;
    }
    if (endp) *endp = (char*)(any ? p : s);
    return result;
}

int atoi(const char* s)          { return (int)strtol(s, NULL, 10); }
long atol(const char* s)         { return strtol(s, NULL, 10); }
long long atoll(const char* s) {
    /* strtoll сделаем потом, пока через strtol — на x86_64 long == 64-bit. */
    return (long long)strtol(s, NULL, 10);
}

int  abs(int n)            { return n < 0 ? -n : n; }
long labs(long n)          { return n < 0 ? -n : n; }
long long llabs(long long n){ return n < 0 ? -n : n; }

/* ---------- rand: linear congruential ---------- */

static uint32_t rng_state = 1;

void srand(unsigned seed) { rng_state = seed ? seed : 1; }

int rand(void) {
    /* Numerical Recipes constants. Не криптографически стойкий — и не надо. */
    rng_state = rng_state * 1664525u + 1013904223u;
    return (int)(rng_state & RAND_MAX);
}

/* ---------- qsort ----------
 * Простейший insertion sort — O(n^2). Когда понадобится скорость,
 * заменим на introsort или heap-sort. Для конфигов и сортировок
 * процессов десятка элементов хватит за глаза.
 */
static void swap_bytes(void* a, void* b, size_t n) {
    uint8_t* x = (uint8_t*)a;
    uint8_t* y = (uint8_t*)b;
    while (n--) {
        uint8_t t = *x; *x = *y; *y = t;
        x++; y++;
    }
}

void qsort(void* base, size_t n, size_t sz,
           int (*cmp)(const void*, const void*)) {
    uint8_t* a = (uint8_t*)base;
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0 && cmp(a + (j-1)*sz, a + j*sz) > 0; j--) {
            swap_bytes(a + (j-1)*sz, a + j*sz, sz);
        }
    }
}

/* ---------- abort/exit ----------
 * Делегируется backend'у. В ядре abort = panic, в userspace = syscall exit. */

extern void __libc_exit_impl(int status) __attribute__((noreturn));

void abort(void) {
    __libc_exit_impl(127);
}

void exit(int status) {
    __libc_exit_impl(status);
}

/* system(cmd) — стандартная функция, делает fork+execve+waitpid.
   У нас пока нет fork+exec — возвращаем -1 с errno=ENOSYS. */
int system(const char* cmd) {
    (void)cmd;
    extern int errno;
    errno = 38;   /* ENOSYS */
    return -1;
}

/* На x86_64 long == long long (оба 64-bit), поэтому strtoll/strtoull
   делегируем к strtol/strtoul. */
long long strtoll(const char* s, char** endp, int base) {
    return (long long)strtol(s, endp, base);
}
unsigned long long strtoull(const char* s, char** endp, int base) {
    return (unsigned long long)strtoul(s, endp, base);
}

/* strtod — парсинг float. Простая реализация: целая часть, дробная
   часть, опциональная экспонента. Достаточно для большинства кода. */
double strtod(const char* s, char** endp) {
    const char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;

    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;

    double val = 0.0;
    /* Целая часть */
    while (*p >= '0' && *p <= '9') {
        val = val * 10.0 + (*p - '0');
        p++;
    }
    /* Дробная часть */
    if (*p == '.') {
        p++;
        double frac = 0.1;
        while (*p >= '0' && *p <= '9') {
            val += (*p - '0') * frac;
            frac *= 0.1;
            p++;
        }
    }
    /* Экспонента */
    if (*p == 'e' || *p == 'E') {
        p++;
        int esign = 1;
        if (*p == '-') { esign = -1; p++; }
        else if (*p == '+') p++;
        int exp = 0;
        while (*p >= '0' && *p <= '9') {
            exp = exp * 10 + (*p - '0');
            p++;
        }
        double mult = 1.0;
        for (int i = 0; i < exp; i++) mult *= 10.0;
        if (esign < 0) val /= mult;
        else           val *= mult;
    }

    if (endp) *endp = (char*)p;
    return sign * val;
}

float strtof(const char* s, char** endp) {
    return (float)strtod(s, endp);
}

long double strtold(const char* s, char** endp) {
    return (long double)strtod(s, endp);
}

double atof(const char* s) {
    return strtod(s, NULL);
}

/* realpath — упрощённо: копируем path как есть (без симлинков и .. в нашей FS).
   Если resolved==NULL, выделяем буфер. */
char* realpath(const char* path, char* resolved) {
    extern void* __libc_malloc_impl(size_t);
    if (!path) return NULL;
    size_t len = strlen(path);
    char* out = resolved ? resolved : (char*)__libc_malloc_impl(len + 1);
    if (!out) return NULL;
    memcpy(out, path, len + 1);
    return out;
}

/* ---- glob.h: упрощённая заглушка ----
   chibicc использует glob для поиска системных include-путей. В RaxOS
   мы возвращаем "нет совпадений" — пути задаются явно через -I. */
#include <glob.h>
int glob(const char* pattern, int flags, int (*errfunc)(const char*, int), glob_t* pglob) {
    (void)pattern; (void)flags; (void)errfunc;
    if (pglob) { pglob->gl_pathc = 0; pglob->gl_pathv = 0; pglob->gl_offs = 0; }
    return GLOB_NOMATCH;
}
void globfree(glob_t* pglob) { (void)pglob; }

/* ---- atexit (упрощённо) ---- */
#define ATEXIT_MAX 32
static void (*_atexit_fns[ATEXIT_MAX])(void);
static int _atexit_n = 0;
int atexit(void (*fn)(void)) {
    if (_atexit_n >= ATEXIT_MAX) return -1;
    _atexit_fns[_atexit_n++] = fn;
    return 0;
}
void __run_atexit(void) {
    for (int i = _atexit_n - 1; i >= 0; i--) if (_atexit_fns[i]) _atexit_fns[i]();
}
