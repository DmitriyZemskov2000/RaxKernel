/*
 * stdlib_env.c — environment variables.
 *
 * environ — глобальный массив указателей на строки "KEY=VALUE",
 * последний элемент = NULL. crt0 инициализирует через
 * __libc_init_environ из стека.
 *
 * getenv ищет первый match по prefix "KEY=".
 * setenv/unsetenv модифицируют массив (в нашем простом случае —
 * выделяем malloc-копию массива при первой записи, чтобы не трогать
 * исходный стек).
 */

#include <stddef.h>
#include <string.h>
#include <stdlib.h>

extern void* __libc_malloc_impl(size_t);
extern void  __libc_free_impl(void*);

char** environ = NULL;
static int env_owned = 0;       /* 1 если environ — наша malloc-копия */

/* Инициализация — вызывается из crt0 _start */
void __libc_init_environ(char** envp) {
    environ = envp;
    env_owned = 0;
}

static int env_count(void) {
    if (!environ) return 0;
    int n = 0;
    while (environ[n]) n++;
    return n;
}

static void env_take_ownership(void) {
    if (env_owned) return;
    int n = env_count();
    char** copy = (char**)__libc_malloc_impl((n + 1) * sizeof(char*));
    if (!copy) return;
    for (int i = 0; i < n; i++) copy[i] = environ[i];
    copy[n] = NULL;
    environ = copy;
    env_owned = 1;
}

char* getenv(const char* name) {
    if (!environ || !name) return NULL;
    size_t nlen = strlen(name);
    for (int i = 0; environ[i]; i++) {
        const char* e = environ[i];
        if (!strncmp(e, name, nlen) && e[nlen] == '=') {
            return (char*)(e + nlen + 1);
        }
    }
    return NULL;
}

int setenv(const char* name, const char* value, int overwrite) {
    if (!name || !value || !*name) return -1;
    if (strchr(name, '=')) return -1;

    /* Проверим, не существует ли */
    char* exist = getenv(name);
    if (exist && !overwrite) return 0;

    env_take_ownership();

    /* Соберём строку "name=value" */
    size_t nlen = strlen(name), vlen = strlen(value);
    char* entry = (char*)__libc_malloc_impl(nlen + 1 + vlen + 1);
    if (!entry) return -1;
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen);
    entry[nlen + 1 + vlen] = '\0';

    /* Заменяем существующую запись или добавляем */
    int n = env_count();
    for (int i = 0; i < n; i++) {
        if (!strncmp(environ[i], name, nlen) && environ[i][nlen] == '=') {
            environ[i] = entry;
            return 0;
        }
    }

    /* Расширяем массив */
    char** new_env = (char**)__libc_malloc_impl((n + 2) * sizeof(char*));
    if (!new_env) { __libc_free_impl(entry); return -1; }
    for (int i = 0; i < n; i++) new_env[i] = environ[i];
    new_env[n] = entry;
    new_env[n + 1] = NULL;
    environ = new_env;
    return 0;
}

int unsetenv(const char* name) {
    if (!name || !*name || strchr(name, '=')) return -1;
    env_take_ownership();
    size_t nlen = strlen(name);
    int n = env_count();
    for (int i = 0; i < n; i++) {
        if (!strncmp(environ[i], name, nlen) && environ[i][nlen] == '=') {
            /* shift вниз */
            for (int j = i; j < n; j++) environ[j] = environ[j + 1];
            return 0;
        }
    }
    return 0;
}

int putenv(char* str) {
    if (!str) return -1;
    char* eq = strchr(str, '=');
    if (!eq) return -1;
    *eq = '\0';
    int r = setenv(str, eq + 1, 1);
    *eq = '=';
    return r;
}
