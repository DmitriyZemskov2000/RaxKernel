/*
 * stdio.h — стандартный ввод-вывод.
 *
 * В ядерном билде:
 *   * printf/puts пишут в VGA+serial через kputs (см. main).
 *   * stdin не используется — будет переопределён в userspace.
 *   * FILE здесь упрощённая — настоящие потоки появятся с VFS.
 */
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _FILE FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

#define EOF (-1)

int snprintf(char* buf, size_t n, const char* fmt, ...);
int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
int sprintf(char* buf, const char* fmt, ...);
int vsprintf(char* buf, const char* fmt, va_list ap);

int printf(const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int fprintf(FILE* stream, const char* fmt, ...);
int vfprintf(FILE* stream, const char* fmt, va_list ap);
int puts(const char* s);
int putchar(int c);

int fputs(const char* s, FILE* stream);
int fputc(int c, FILE* stream);

FILE*  fopen(const char* path, const char* mode);
int    fclose(FILE* f);

int    fseek(FILE* f, long offset, int whence);
long   ftell(FILE* f);
void   rewind(FILE* f);
int    fgetpos(FILE* f, long* pos);
int    fsetpos(FILE* f, const long* pos);
int    remove(const char* path);
FILE*  fdopen(int fd, const char* mode);
FILE*  freopen(const char* path, const char* mode, FILE* stream);
FILE*  open_memstream(char** bufp, size_t* sizep);

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
int    fflush(FILE* f);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f);
int    fgetc(FILE* f);
char*  fgets(char* s, int size, FILE* f);
int    feof(FILE* f);
int    ferror(FILE* f);
void   clearerr(FILE* f);

#ifdef __cplusplus
}
#endif

#endif
