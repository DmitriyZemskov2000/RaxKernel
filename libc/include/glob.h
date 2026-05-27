#ifndef _GLOB_H
#define _GLOB_H
#include <stddef.h>

typedef struct {
    size_t gl_pathc;     /* число найденных путей */
    char** gl_pathv;     /* массив путей */
    size_t gl_offs;
} glob_t;

#define GLOB_NOMATCH  3
#define GLOB_NOSPACE  1
#define GLOB_ABORTED  2

int  glob(const char* pattern, int flags, int (*errfunc)(const char*, int), glob_t* pglob);
void globfree(glob_t* pglob);

#endif
