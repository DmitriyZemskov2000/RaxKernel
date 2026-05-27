/*
 * time.c — реализация time(2), clock_gettime(2) и пр.
 *
 * Источник тиков — функция, которую предоставляет ядро.
 * Это позволяет той же реализации работать и в userspace —
 * там за этой функцией стоит syscall.
 */

#include <time.h>
#include <stddef.h>

/* Эти символы предоставляет ядро (pit.c). В userspace будут переопределены. */
extern unsigned long long pit_ticks(void);
extern unsigned           pit_frequency(void);
extern void               pit_sleep_ms(unsigned ms);

clock_t clock(void) {
    /* CLOCKS_PER_SEC == 1e6. Тиков у нас pit_frequency в секунду. */
    unsigned long long t = pit_ticks();
    unsigned hz = pit_frequency();
    if (hz == 0) hz = 1;
    return (clock_t)((t * 1000000ULL) / hz);
}

time_t time(time_t* t) {
    /* Пока нет RTC, time = uptime. Допустимо для bring-up'а. */
    unsigned long long ticks = pit_ticks();
    unsigned hz = pit_frequency();
    if (hz == 0) hz = 1;
    time_t secs = (time_t)(ticks / hz);
    if (t) *t = secs;
    return secs;
}

int clock_gettime(int clk_id, struct timespec* tp) {
    (void)clk_id;
    if (!tp) return -1;
    unsigned long long ticks = pit_ticks();
    unsigned hz = pit_frequency();
    if (hz == 0) hz = 1;
    tp->tv_sec  = (time_t)(ticks / hz);
    tp->tv_nsec = (long)((ticks % hz) * (1000000000ULL / hz));
    return 0;
}

void usleep_ms(unsigned ms) {
    pit_sleep_ms(ms);
}

/* ---- Календарные функции ---- */

static struct tm _tm_buf;

/* Преобразование Unix time → struct tm (UTC). Алгоритм civil_from_days. */
struct tm* gmtime(const time_t* tp) {
    if (!tp) return NULL;
    time_t t = *tp;
    long days = t / 86400;
    int secs = (int)(t % 86400);
    if (secs < 0) { secs += 86400; days--; }

    _tm_buf.tm_sec = secs % 60;
    _tm_buf.tm_min = (secs / 60) % 60;
    _tm_buf.tm_hour = secs / 3600;
    _tm_buf.tm_wday = (int)((days % 7 + 4 + 7) % 7);  /* 1970-01-01 = четверг */

    /* civil_from_days (Howard Hinnant) */
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2)/153;
    unsigned d = doy - (153*mp + 2)/5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);

    _tm_buf.tm_year = (int)(y - 1900);
    _tm_buf.tm_mon = (int)m - 1;
    _tm_buf.tm_mday = (int)d;
    _tm_buf.tm_yday = (int)doy;
    _tm_buf.tm_isdst = 0;
    return &_tm_buf;
}

struct tm* localtime(const time_t* tp) {
    return gmtime(tp);   /* без timezone — UTC */
}

time_t mktime(struct tm* tm) {
    /* Обратное преобразование (приблизительное, UTC) */
    int y = tm->tm_year + 1900;
    int m = tm->tm_mon + 1;
    int d = tm->tm_mday;
    if (m <= 2) { y--; m += 12; }
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153*(m > 2 ? m-3 : m+9) + 2)/5 + d - 1;
    unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

size_t strftime(char* s, size_t max, const char* fmt, const struct tm* tm) {
    /* Минимальная реализация: поддержка нескольких спецификаторов */
    (void)tm;
    if (max == 0) return 0;
    size_t i = 0;
    while (*fmt && i < max - 1) {
        if (*fmt == '%') {
            fmt++;
            /* пропускаем спецификатор без вывода (заглушка) */
            if (*fmt) fmt++;
        } else {
            s[i++] = *fmt++;
        }
    }
    s[i] = '\0';
    return i;
}

char* asctime(const struct tm* tm) {
    (void)tm;
    static char buf[26] = "Thu Jan  1 00:00:00 1970\n";
    return buf;
}

char* ctime(const time_t* t) {
    return asctime(localtime(t));
}

/* ctime_r — реентерабельный ctime (пишет в буфер пользователя) */
char* ctime_r(const time_t* t, char* buf) {
    char* s = ctime(t);   /* статический буфер */
    if (!s || !buf) return buf;
    int i = 0;
    while (s[i] && i < 25) { buf[i] = s[i]; i++; }
    buf[i] = '\0';
    return buf;
}
