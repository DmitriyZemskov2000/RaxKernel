/*
 * time.h — работа со временем.
 *
 * Минимальный POSIX-набор. Источник времени в ядре — счётчик
 * тиков PIT (см. pit_ticks). После добавления RTC/TSC будет точнее.
 */
#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long time_t;
typedef long suseconds_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 1000000L

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

/* struct timeval определён в <sys/time.h> */

struct tm {
    int tm_sec;    /* 0-60 */
    int tm_min;    /* 0-59 */
    int tm_hour;   /* 0-23 */
    int tm_mday;   /* 1-31 */
    int tm_mon;    /* 0-11 */
    int tm_year;   /* годы с 1900 */
    int tm_wday;   /* 0-6, воскресенье=0 */
    int tm_yday;   /* 0-365 */
    int tm_isdst;
};

struct tm* localtime(const time_t* t);
struct tm* gmtime(const time_t* t);
time_t     mktime(struct tm* tm);
size_t     strftime(char* s, size_t max, const char* fmt, const struct tm* tm);
char*      asctime(const struct tm* tm);
char*      ctime(const time_t* t);
double     difftime(time_t a, time_t b);

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

clock_t clock(void);
time_t  time(time_t* t);
int     clock_gettime(int clk_id, struct timespec* tp);

/* Низкоуровневая утилита — спать N миллисекунд */
void usleep_ms(unsigned ms);

#ifdef __cplusplus
}
#endif

char* ctime_r(const time_t* t, char* buf);
#endif
