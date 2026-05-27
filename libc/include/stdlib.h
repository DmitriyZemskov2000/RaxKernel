/*
 * stdlib.h — общие утилиты.
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#define RAND_MAX 0x7FFFFFFF

void*  malloc(size_t size);
void*  calloc(size_t nmemb, size_t size);
void*  realloc(void* ptr, size_t size);
void   free(void* ptr);

int    atoi(const char* s);
long   atol(const char* s);
long long atoll(const char* s);
long   strtol(const char* s, char** endp, int base);
unsigned long strtoul(const char* s, char** endp, int base);
long long strtoll(const char* s, char** endp, int base);
unsigned long long strtoull(const char* s, char** endp, int base);
double strtod(const char* s, char** endp);
float  strtof(const char* s, char** endp);
long double strtold(const char* s, char** endp);
double atof(const char* s);

int    abs(int n);
long   labs(long n);
long long llabs(long long n);

int    rand(void);
void   srand(unsigned seed);

void   qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*));

void   abort(void) __attribute__((noreturn));
void   exit(int status) __attribute__((noreturn));

/* environment */
extern char** environ;
char*  getenv(const char* name);
int    setenv(const char* name, const char* value, int overwrite);
int    unsetenv(const char* name);
int    putenv(char* str);

/* system / exec helpers (для будущего: gcc вызывает system(),
   которая делает fork+execve+waitpid) */
int    system(const char* cmd);

/* misc */
int    atoi(const char* s);
long   atol(const char* s);

/* Случайные числа (LCG) — RAND_MAX уже определён выше */
int    rand(void);
void   srand(unsigned seed);

#ifdef __cplusplus
}
#endif

#endif

/* Path canonicalization (упрощённая) */
char* realpath(const char* path, char* resolved);
int mkstemp(char* template);
int atexit(void (*fn)(void));
