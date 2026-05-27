/*
 * string.c — реализация <string.h>.
 *
 * Прямолинейная байтовая реализация. Не используем SIMD —
 * у нас в ядре SSE отключён. Это медленнее musl/glibc, но для
 * первых итераций ОС цены нет: на десятках мегабайт ввода-вывода
 * разница незаметна, а отлаживать байтовый цикл намного проще.
 */

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void* memset(void* dst, int val, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    uint8_t v = (uint8_t)val;
    while (n--) *d++ = v;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;
    uint8_t v = (uint8_t)c;
    while (n--) {
        if (*p == v) return (void*)p;
        p++;
    }
    return NULL;
}

size_t strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char* s, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen && s[i]) i++;
    return i;
}

char* strcpy(char* dst, const char* src) {
    char* r = dst;
    while ((*dst++ = *src++)) {}
    return r;
}

char* strncpy(char* dst, const char* src, size_t n) {
    char* r = dst;
    while (n && (*dst++ = *src++)) n--;
    /* стандарт требует добивать нулями */
    while (n--) *dst++ = '\0';
    return r;
}

char* strcat(char* dst, const char* src) {
    char* r = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++)) {}
    return r;
}

char* strncat(char* dst, const char* src, size_t n) {
    char* r = dst;
    while (*dst) dst++;
    while (n-- && *src) *dst++ = *src++;
    *dst = '\0';
    return r;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

char* strchr(const char* s, int c) {
    char v = (char)c;
    while (*s) {
        if (*s == v) return (char*)s;
        s++;
    }
    if (v == 0) return (char*)s;
    return NULL;
}

char* strrchr(const char* s, int c) {
    char v = (char)c;
    const char* last = NULL;
    while (*s) {
        if (*s == v) last = s;
        s++;
    }
    if (v == 0) return (char*)s;
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    size_t nl = strlen(needle);
    for (; *haystack; haystack++) {
        if (!memcmp(haystack, needle, nl)) return (char*)haystack;
    }
    return NULL;
}

size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char* strpbrk(const char* s, const char* accept) {
    for (; *s; s++) {
        if (strchr(accept, *s)) return (char*)s;
    }
    return NULL;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* r = (char*)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

/*
 * strtok_r — re-entrant tokenizer. Состояние хранится в *saveptr.
 * Никогда не делать strtok без _r в нашей среде — он использует
 * глобал, что неприемлемо с тем планировщиком, который у нас есть.
 */
char* strtok_r(char* s, const char* delim, char** saveptr) {
    char* p;
    if (s) p = s;
    else if (saveptr && *saveptr) p = *saveptr;
    else return NULL;

    /* Пропускаем разделители */
    p += strspn(p, delim);
    if (*p == '\0') {
        if (saveptr) *saveptr = NULL;
        return NULL;
    }

    char* tok = p;
    p = strpbrk(tok, delim);
    if (p) {
        *p = '\0';
        if (saveptr) *saveptr = p + 1;
    } else {
        if (saveptr) *saveptr = NULL;
    }
    return tok;
}

/* Простейший strerror — без таблиц для всех POSIX-кодов;
   добавим по мере появления реальных ошибок. */
const char* strerror(int errnum) {
    switch (errnum) {
        case 0:   return "Success";
        case 1:   return "Operation not permitted";
        case 2:   return "No such file or directory";
        case 5:   return "I/O error";
        case 9:   return "Bad file descriptor";
        case 11:  return "Try again";
        case 12:  return "Out of memory";
        case 13:  return "Permission denied";
        case 14:  return "Bad address";
        case 22:  return "Invalid argument";
        case 38:  return "Function not implemented";
        default:  return "Unknown error";
    }
}

/* ---- strings.h: case-insensitive сравнение ---- */
static int _tolower_c(int c){ return (c>='A'&&c<='Z') ? c+32 : c; }

int strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        int d = _tolower_c((unsigned char)*a) - _tolower_c((unsigned char)*b);
        if (d) return d;
        a++; b++;
    }
    return _tolower_c((unsigned char)*a) - _tolower_c((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    while (n && *a && *b) {
        int d = _tolower_c((unsigned char)*a) - _tolower_c((unsigned char)*b);
        if (d) return d;
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return _tolower_c((unsigned char)*a) - _tolower_c((unsigned char)*b);
}

/* ---- libgen.h: basename/dirname ---- */
char* basename(char* path) {
    if (!path || !*path) return ".";
    char* p = path;
    char* last = path;
    for (; *p; p++) if (*p == '/') last = p + 1;
    return last;
}

char* dirname(char* path) {
    if (!path || !*path) return ".";
    char* p = path;
    char* last_slash = NULL;
    for (; *p; p++) if (*p == '/') last_slash = p;
    if (!last_slash) return ".";
    if (last_slash == path) { path[1] = '\0'; return path; }
    *last_slash = '\0';
    return path;
}

/* ---- strndup ---- */
char* strndup(const char* s, size_t n) {
    extern void* __libc_malloc_impl(size_t);
    size_t len = 0;
    while (len < n && s[len]) len++;
    char* r = (char*)__libc_malloc_impl(len + 1);
    if (!r) return 0;
    memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

/* ---- strtok ---- */
char* strtok(char* str, const char* delim) {
    static char* save;
    if (str) save = str;
    if (!save) return 0;
    /* пропускаем ведущие разделители */
    while (*save) {
        const char* d = delim; int is_d = 0;
        for (; *d; d++) if (*save == *d) { is_d = 1; break; }
        if (!is_d) break;
        save++;
    }
    if (!*save) { save = 0; return 0; }
    char* start = save;
    while (*save) {
        const char* d = delim;
        for (; *d; d++) if (*save == *d) { *save++ = '\0'; return start; }
        save++;
    }
    save = 0;
    return start;
}
