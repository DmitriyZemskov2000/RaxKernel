/*
 * stdio.c — реализация форматного вывода и stdio API.
 *
 * Главный кусок — vsnprintf. Это сердце libc: и printf, и
 * snprintf, и fprintf — все строятся над ним. Поддерживаем:
 *
 *   Спецификаторы: %c %s %d %i %u %x %X %o %p %% %n
 *   Флаги:         '-' (left-align), '+', ' ', '#', '0'
 *   Ширина:        число или '*'
 *   Точность:      .число или .*
 *   Длина:         h, hh, l, ll, z, t
 *
 * Float (%f %e %g) не реализуем — в ядре нет SSE и FPU нам не нужны.
 *
 * Алгоритм: пишем в callback-функцию (sink). snprintf'овый sink
 * пишет в буфер, printf-овый зовёт __libc_write_impl. Это позволяет
 * избавиться от промежуточного буфера для printf.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

/* ---- Низкоуровневый вывод (поставляется ядром) ---- */
extern void __libc_write_impl(const char* s, size_t n);
extern void* __libc_malloc_impl(size_t n);
extern void  __libc_free_impl(void* p);

/* ---------- Sink-абстракция ---------- */

typedef struct {
    char*  buf;          /* буфер вывода (для *snprintf) или NULL для printf */
    size_t pos;          /* сколько уже "записано" в буфер */
    size_t cap;          /* размер буфера (включая место под '\0') */
    size_t total;        /* общее число СГЕНЕРИРОВАННЫХ байт (включая обрезанные) */
} sink_t;

static void sink_write(sink_t* s, const char* data, size_t n) {
    s->total += n;
    if (s->buf) {
        if (s->cap == 0) return;
        size_t free_space = (s->pos < s->cap - 1) ? (s->cap - 1 - s->pos) : 0;
        size_t to_copy = n < free_space ? n : free_space;
        if (to_copy) {
            memcpy(s->buf + s->pos, data, to_copy);
            s->pos += to_copy;
        }
    } else {
        __libc_write_impl(data, n);
    }
}

static void sink_putc(sink_t* s, char c) {
    sink_write(s, &c, 1);
}

static void sink_repeat(sink_t* s, char c, size_t n) {
    char buf[16];
    memset(buf, c, sizeof(buf));
    while (n) {
        size_t chunk = n > sizeof(buf) ? sizeof(buf) : n;
        sink_write(s, buf, chunk);
        n -= chunk;
    }
}

/* ---------- Форматирование чисел ---------- */

/* Возвращает число цифр; пишет их в buf в обратном порядке.
   buf должен быть достаточно большим (24 байт хватит для 64-бит). */
static int u64_to_digits(uint64_t v, unsigned base, int upper, char* buf) {
    const char* digits_lo = "0123456789abcdef";
    const char* digits_hi = "0123456789ABCDEF";
    const char* d = upper ? digits_hi : digits_lo;
    int n = 0;
    if (v == 0) { buf[n++] = '0'; return n; }
    while (v) {
        buf[n++] = d[v % base];
        v /= base;
    }
    return n;
}

/* ---------- Парсинг и применение спецификатора ---------- */

typedef struct {
    int left;        /* '-' */
    int plus;        /* '+' */
    int space;       /* ' ' */
    int alt;         /* '#' */
    int zero;        /* '0' */
    int width;
    int prec;        /* -1 если не задана */
    int len;         /* 0=int, 1='h', 2='hh', 3='l', 4='ll', 5='z', 6='t' */
} fmt_t;

static const char* parse_flags(const char* p, fmt_t* f) {
    for (;; p++) {
        switch (*p) {
            case '-': f->left  = 1; continue;
            case '+': f->plus  = 1; continue;
            case ' ': f->space = 1; continue;
            case '#': f->alt   = 1; continue;
            case '0': f->zero  = 1; continue;
        }
        return p;
    }
}

/* Парсинг числа или '*'. Делаем макросом, чтобы va_arg напрямую
   работал с локальной ap-переменной do_vsnprintf'а. */
#define PARSE_INT_OR_STAR(p_, out_, ap_) do { \
    if (*(p_) == '*') { (out_) = va_arg(ap_, int); (p_)++; } \
    else { \
        int _n = 0; \
        while (*(p_) >= '0' && *(p_) <= '9') { _n = _n*10 + (*(p_)-'0'); (p_)++; } \
        (out_) = _n; \
    } \
} while (0)

static void emit_padded(sink_t* sk, const fmt_t* f,
                        const char* prefix, int prefix_len,
                        const char* body,   int body_len,
                        int zero_pad_count)
{
    int content_len = prefix_len + zero_pad_count + body_len;
    int width = f->width;
    int pad = (width > content_len) ? (width - content_len) : 0;

    if (!f->left && !(f->zero && f->prec < 0)) {
        sink_repeat(sk, ' ', (size_t)pad);
    }
    sink_write(sk, prefix, (size_t)prefix_len);
    if (!f->left && f->zero && f->prec < 0) {
        sink_repeat(sk, '0', (size_t)pad);
    }
    sink_repeat(sk, '0', (size_t)zero_pad_count);
    sink_write(sk, body, (size_t)body_len);
    if (f->left) {
        sink_repeat(sk, ' ', (size_t)pad);
    }
}

static void format_uint(sink_t* sk, fmt_t* f, uint64_t v,
                        unsigned base, int upper, int is_signed_neg) {
    char tmp[24];
    int n = u64_to_digits(v, base, upper, tmp);

    /* Реверсируем для удобства */
    char body[24];
    for (int i = 0; i < n; i++) body[i] = tmp[n - 1 - i];

    /* Точность: минимум цифр. При prec=0 и v=0 — пустота. */
    int zero_pad = 0;
    if (f->prec >= 0) {
        f->zero = 0;     /* при заданной точности '0'-флаг игнорируется */
        if (f->prec > n) zero_pad = f->prec - n;
        if (f->prec == 0 && v == 0) n = 0;
    }

    /* Префикс */
    char prefix[3];
    int plen = 0;
    if (is_signed_neg) prefix[plen++] = '-';
    else if (f->plus)  prefix[plen++] = '+';
    else if (f->space) prefix[plen++] = ' ';

    if (f->alt && v != 0) {
        if (base == 16) { prefix[plen++] = '0'; prefix[plen++] = upper ? 'X' : 'x'; }
        else if (base == 8 && (zero_pad == 0 || (f->prec >= 0 && f->prec <= n)))
            prefix[plen++] = '0';
    }

    emit_padded(sk, f, prefix, plen, n ? body : "", n, zero_pad);
}

static int do_vsnprintf(sink_t* sk, const char* fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') { sink_putc(sk, *fmt++); continue; }
        fmt++;

        fmt_t f = {0, 0, 0, 0, 0, 0, -1, 0};
        fmt = parse_flags(fmt, &f);

        /* Width */
        if (*fmt == '*' || (*fmt >= '0' && *fmt <= '9')) {
            PARSE_INT_OR_STAR(fmt, f.width, ap);
            if (f.width < 0) { f.left = 1; f.width = -f.width; }
        }

        /* Precision */
        if (*fmt == '.') {
            fmt++;
            PARSE_INT_OR_STAR(fmt, f.prec, ap);
            if (f.prec < 0) f.prec = 0;
        }

        /* Length */
        if (*fmt == 'h') { fmt++; f.len = 1; if (*fmt == 'h') { fmt++; f.len = 2; } }
        else if (*fmt == 'l') { fmt++; f.len = 3; if (*fmt == 'l') { fmt++; f.len = 4; } }
        else if (*fmt == 'z') { fmt++; f.len = 5; }
        else if (*fmt == 't') { fmt++; f.len = 6; }

        char ch = *fmt++;
        switch (ch) {
            case 'c': {
                char c = (char)va_arg(ap, int);
                emit_padded(sk, &f, "", 0, &c, 1, 0);
                break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                int len = (int)strnlen(s, f.prec < 0 ? (size_t)-1 : (size_t)f.prec);
                emit_padded(sk, &f, "", 0, s, len, 0);
                break;
            }
            case 'd': case 'i': {
                int64_t v;
                switch (f.len) {
                    case 4: v = va_arg(ap, long long); break;
                    case 3: case 5: case 6: v = va_arg(ap, long); break;
                    default: v = va_arg(ap, int); break;
                }
                int neg = v < 0;
                uint64_t uv = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
                format_uint(sk, &f, uv, 10, 0, neg);
                break;
            }
            case 'u': {
                uint64_t v;
                switch (f.len) {
                    case 4: v = va_arg(ap, unsigned long long); break;
                    case 3: case 5: case 6: v = va_arg(ap, unsigned long); break;
                    default: v = va_arg(ap, unsigned); break;
                }
                format_uint(sk, &f, v, 10, 0, 0);
                break;
            }
            case 'x': case 'X': {
                uint64_t v;
                switch (f.len) {
                    case 4: v = va_arg(ap, unsigned long long); break;
                    case 3: case 5: case 6: v = va_arg(ap, unsigned long); break;
                    default: v = va_arg(ap, unsigned); break;
                }
                format_uint(sk, &f, v, 16, ch == 'X', 0);
                break;
            }
            case 'o': {
                uint64_t v;
                switch (f.len) {
                    case 4: v = va_arg(ap, unsigned long long); break;
                    case 3: case 5: case 6: v = va_arg(ap, unsigned long); break;
                    default: v = va_arg(ap, unsigned); break;
                }
                format_uint(sk, &f, v, 8, 0, 0);
                break;
            }
            case 'p': {
                void* p = va_arg(ap, void*);
                f.alt = 1;
                format_uint(sk, &f, (uint64_t)(uintptr_t)p, 16, 0, 0);
                break;
            }
            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G': {
                /*
                 * Простая реализация %f: scale + integer arithmetic.
                 * Точность по умолчанию 6. NaN/Inf обрабатываем.
                 *
                 * %e и %g пока упрощены до %f с авто-precision.
                 * Полная реализация значительно сложнее (правильное
                 * округление, экспоненциальная нотация).
                 */
                double v = va_arg(ap, double);
                int prec = (f.prec >= 0) ? f.prec : 6;
                if (prec > 18) prec = 18;

                /* NaN / Inf */
                union { double d; unsigned long long u; } un;
                un.d = v;
                int sign = (un.u >> 63) & 1;
                unsigned long long exp_bits = (un.u >> 52) & 0x7FF;
                unsigned long long frac_bits = un.u & 0xFFFFFFFFFFFFFULL;
                if (exp_bits == 0x7FF) {
                    const char* s = frac_bits ? "nan" : (sign ? "-inf" : "inf");
                    emit_padded(sk, &f, "", 0, s, (int)strlen(s), 0);
                    break;
                }

                if (sign) { v = -v; }

                /* Целая часть */
                unsigned long long intpart = (unsigned long long)v;
                double frac = v - (double)intpart;

                /* Дробная часть × 10^prec, округление half-away */
                unsigned long long scale = 1;
                for (int i = 0; i < prec; i++) scale *= 10;
                unsigned long long fracint = (unsigned long long)(frac * scale + 0.5);
                /* Перенос при округлении */
                if (fracint >= scale) {
                    intpart++;
                    fracint -= scale;
                }

                /* Печатаем: [-]intpart.fracint */
                char body[64];
                int bi = 0;

                /* Sign / plus / space prefix */
                if (sign)      body[bi++] = '-';
                else if (f.plus)  body[bi++] = '+';
                else if (f.space) body[bi++] = ' ';

                /* intpart digits */
                char ibuf[24]; int icnt = 0;
                if (intpart == 0) ibuf[icnt++] = '0';
                else while (intpart) { ibuf[icnt++] = '0' + (intpart % 10); intpart /= 10; }
                while (icnt-- > 0) body[bi++] = ibuf[icnt];

                if (prec > 0) {
                    body[bi++] = '.';
                    char fbuf[24];
                    int fcnt = 0;
                    unsigned long long t = fracint;
                    for (int i = 0; i < prec; i++) {
                        fbuf[fcnt++] = '0' + (t % 10);
                        t /= 10;
                    }
                    while (fcnt-- > 0) body[bi++] = fbuf[fcnt];
                } else if (f.alt) {
                    body[bi++] = '.';
                }

                emit_padded(sk, &f, "", 0, body, bi, 0);
                break;
            }
            case 'n': {
                int* dst = va_arg(ap, int*);
                if (dst) *dst = (int)sk->total;
                break;
            }
            case '%':
                sink_putc(sk, '%');
                break;
            default:
                /* Неизвестный спецификатор — выводим как есть */
                sink_putc(sk, '%');
                sink_putc(sk, ch);
        }
    }
    return (int)sk->total;
}

/* ---------- Публичный API ---------- */

int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) {
    sink_t sk = { buf, 0, n, 0 };
    int r = do_vsnprintf(&sk, fmt, ap);
    if (buf && n > 0) buf[sk.pos] = '\0';
    return r;
}

int snprintf(char* buf, size_t n, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char* buf, const char* fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char* fmt, va_list ap) {
    sink_t sk = { NULL, 0, 0, 0 };
    return do_vsnprintf(&sk, fmt, ap);
}

int printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char* s) {
    size_t n = strlen(s);
    __libc_write_impl(s, n);
    __libc_write_impl("\n", 1);
    return (int)n + 1;
}

int putchar(int c) {
    char ch = (char)c;
    __libc_write_impl(&ch, 1);
    return (unsigned char)ch;
}

/* ---------- FILE* — настоящая буферизованная реализация ----------
 *
 * Поля:
 *   fd          — POSIX file descriptor (либо -1 для stub stdout)
 *   buf         — внутренний буфер
 *   buf_pos     — текущая позиция чтения в буфере
 *   buf_size    — сколько байт реально лежит в буфере (для read mode)
 *   buf_cap     — размер буфера
 *   eof, error  — флаги
 *   mode        — 'r', 'w' и т.п.
 *
 * stdout/stderr — без fd, пишут через __libc_write_impl.
 * stdin тоже stub (нет клавиатуры в этой итерации).
 *
 * Через эту инфраструктуру:
 *   fopen открывает файл, выделяет FILE*+buffer.
 *   fread читает кусками по BUFSIZ из read() в буфер,
 *         потом отдаёт пользователю.
 *   fwrite копирует в buffer, при заполнении сбрасывает write().
 *   fflush сбрасывает write buffer.
 *
 * Используется в обоих режимах:
 *   - kernel libc: backend в kernel/backend.c делает kputs_raw,
 *     fd-обращения уйдут в vfs_read/vfs_write через __libc_open/__libc_read_fd
 *   - userspace libc: backend делает syscall'ы; fopen/fread зовут
 *     open()/read() — поэтому используем хуки __libc_open_impl и т.п.
 *
 * Чтобы не плодить backend'ов, FILE* в данной итерации использует
 * фиксированный набор хуков. Они объявляются ниже как weak, чтобы
 * каждый backend мог их перекрыть.
 */

#define BUFSIZ 1024

struct _FILE {
    int    fd;             /* -1 для stub-режима */
    int    eof;
    int    error;
    int    mode;           /* 'r' = чтение, 'w' = запись */
    char*  buf;
    size_t buf_pos;
    size_t buf_size;
    size_t buf_cap;
    char   tinybuf[BUFSIZ];  /* встроенный, чтобы не делать malloc */
    /* open_memstream: запись в растущий буфер в памяти */
    char**  mem_bufp;
    size_t* mem_sizep;
    char*   mem_buf;
    size_t  mem_len;
    size_t  mem_cap;
};

/* ---- Хуки backend'а для fd-операций ----
 * Если backend не переопределил, fopen не работает (NULL),
 * но stdout/stderr через __libc_write_impl ещё пишут. */
__attribute__((weak)) int  __libc_open_impl(const char* path, int flags)         { (void)path; (void)flags; return -1; }
__attribute__((weak)) long __libc_read_impl(int fd, void* buf, size_t n)         { (void)fd; (void)buf; (void)n; return -1; }
__attribute__((weak)) long __libc_write_fd_impl(int fd, const void* buf, size_t n){ (void)fd; (void)buf; (void)n; return -1; }
__attribute__((weak)) int  __libc_close_impl(int fd)                              { (void)fd; return -1; }

/* stdout/stderr stub'ы: fd = -1, mode = 'w', буфер пустой.
   Запись через них идёт по fast-path в __libc_write_impl. */
static struct _FILE _stdin  = { .fd = -1, .mode = 'r' };
static struct _FILE _stdout = { .fd = -1, .mode = 'w' };
static struct _FILE _stderr = { .fd = -1, .mode = 'w' };
FILE* stdin  = &_stdin;
FILE* stdout = &_stdout;
FILE* stderr = &_stderr;

/* ---- Внутренний "вылив" буфера ---- */
static void file_flush_write(FILE* f) {
    if (f->buf_size == 0) return;
    if (f->fd >= 0) {
        __libc_write_fd_impl(f->fd, f->buf, f->buf_size);
    } else {
        __libc_write_impl(f->buf, f->buf_size);
    }
    f->buf_size = 0;
    f->buf_pos = 0;
}

/* ---- fopen ---- */
FILE* fopen(const char* path, const char* mode) {
    if (!path || !mode) return NULL;
    int flags = 0;
    int m = mode[0];
    if (m == 'r')      flags = 0;           /* O_RDONLY */
    else if (m == 'w') flags = 1 | 0x40 | 0x200;  /* O_WRONLY|O_CREAT|O_TRUNC */
    else if (m == 'a') flags = 1 | 0x40 | 0x400;
    else return NULL;

    int fd = __libc_open_impl(path, flags);
    if (fd < 0) return NULL;

    FILE* f = (FILE*)__libc_malloc_impl(sizeof(*f));
    if (!f) { __libc_close_impl(fd); return NULL; }
    f->fd = fd;
    f->eof = 0;
    f->error = 0;
    f->mode = m;
    f->buf = f->tinybuf;
    f->buf_pos = 0;
    f->buf_size = 0;
    f->buf_cap = BUFSIZ;
    return f;
}

int fclose(FILE* f) {
    if (!f || f == &_stdin || f == &_stdout || f == &_stderr) return -1;
    if (f->mode == 'w' || f->mode == 'a') file_flush_write(f);
    int r = (f->fd >= 0) ? __libc_close_impl(f->fd) : 0;
    __libc_free_impl(f);
    return r;
}

int fflush(FILE* f) {
    if (!f) return 0;
    if (f->mode == 'w' || f->mode == 'a') file_flush_write(f);
    return 0;
}

/* ---- Чтение ---- */
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (!f || f->fd < 0) return 0;
    size_t total = size * nmemb;
    char* dst = (char*)ptr;
    size_t got = 0;

    while (got < total) {
        /* Сначала отдадим то, что уже в буфере */
        size_t avail = f->buf_size - f->buf_pos;
        if (avail > 0) {
            size_t need = total - got;
            size_t cnt = avail < need ? avail : need;
            memcpy(dst + got, f->buf + f->buf_pos, cnt);
            f->buf_pos += cnt;
            got += cnt;
            continue;
        }
        /* Буфер пуст — читаем порцию из fd */
        long r = __libc_read_impl(f->fd, f->buf, f->buf_cap);
        if (r <= 0) {
            if (r == 0) f->eof = 1; else f->error = 1;
            break;
        }
        f->buf_size = (size_t)r;
        f->buf_pos  = 0;
    }
    return size ? got / size : 0;
}

int fgetc(FILE* f) {
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) return EOF;
    return c;
}

char* fgets(char* s, int size, FILE* f) {
    if (size <= 0) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

/* ---- Запись ---- */
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (!f) return 0;
    size_t total = size * nmemb;
    const char* src = (const char*)ptr;

    /* open_memstream: пишем в растущий буфер в памяти */
    if (f->mode == 'm') {
        extern void* __libc_realloc_impl(void*, size_t);
        while (f->mem_len + total + 1 > f->mem_cap) {
            size_t ncap = f->mem_cap * 2;
            char* nb = (char*)__libc_realloc_impl(f->mem_buf, ncap);
            if (!nb) return 0;
            f->mem_buf = nb; f->mem_cap = ncap;
        }
        memcpy(f->mem_buf + f->mem_len, src, total);
        f->mem_len += total;
        f->mem_buf[f->mem_len] = '\0';
        if (f->mem_bufp) *f->mem_bufp = f->mem_buf;
        if (f->mem_sizep) *f->mem_sizep = f->mem_len;
        return nmemb;
    }

    /* stdout/stderr stub — пишем сразу через __libc_write_impl */
    if (f->fd < 0) {
        __libc_write_impl(src, total);
        return nmemb;
    }

    /* Буферизованный режим */
    size_t written = 0;
    while (written < total) {
        size_t room = f->buf_cap - f->buf_size;
        if (room == 0) {
            file_flush_write(f);
            continue;
        }
        size_t need = total - written;
        size_t cnt = room < need ? room : need;
        memcpy(f->buf + f->buf_size, src + written, cnt);
        f->buf_size += cnt;
        written += cnt;
    }
    return size ? written / size : 0;
}

int fputs(const char* s, FILE* stream) {
    size_t n = strlen(s);
    fwrite(s, 1, n, stream);
    return (int)n;
}

int fputc(int c, FILE* stream) {
    unsigned char ch = (unsigned char)c;
    fwrite(&ch, 1, 1, stream);
    return c;
}

int feof(FILE* f) { return f ? f->eof : 0; }
int ferror(FILE* f) { return f ? f->error : 0; }
void clearerr(FILE* f) { if (f) { f->eof = 0; f->error = 0; } }

int vfprintf(FILE* stream, const char* fmt, va_list ap) {
    /* Простейшая реализация: рендерим в temp buffer, потом fwrite.
       Не оптимально, но честно работает для буферизованных FILE. */
    char tmp[2048];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n > 0) {
        size_t cnt = (size_t)n;
        if (cnt > sizeof(tmp) - 1) cnt = sizeof(tmp) - 1;
        fwrite(tmp, 1, cnt, stream);
    }
    return n;
}

int fprintf(FILE* stream, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stream, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- fseek/ftell/rewind ---- */
__attribute__((weak)) long __libc_lseek_impl(int fd, long off, int whence) {
    (void)fd; (void)off; (void)whence; return -1;
}

int fseek(FILE* f, long offset, int whence) {
    if (!f || f->fd < 0) return -1;
    /* Сбросим буфер чтения */
    f->buf_pos = 0;
    f->buf_size = 0;
    f->eof = 0;
    long r = __libc_lseek_impl(f->fd, offset, whence);
    return (r < 0) ? -1 : 0;
}

long ftell(FILE* f) {
    if (!f || f->fd < 0) return -1;
    /* lseek(fd, 0, SEEK_CUR) возвращает текущую позицию.
       Корректируем на непрочитанный буфер. */
    long pos = __libc_lseek_impl(f->fd, 0, 1 /*SEEK_CUR*/);
    if (pos < 0) return -1;
    /* вычитаем то, что в буфере ещё не отдано пользователю */
    pos -= (long)(f->buf_size - f->buf_pos);
    return pos;
}

void rewind(FILE* f) {
    fseek(f, 0, 0 /*SEEK_SET*/);
    if (f) f->error = 0;
}

int fgetpos(FILE* f, long* pos) {
    if (!pos) return -1;
    long p = ftell(f);
    if (p < 0) return -1;
    *pos = p;
    return 0;
}

int fsetpos(FILE* f, const long* pos) {
    if (!pos) return -1;
    return fseek(f, *pos, 0);
}

/* ---- remove ---- */
__attribute__((weak)) int __libc_unlink_impl(const char* path) {
    (void)path; return -1;
}
int remove(const char* path) {
    return __libc_unlink_impl(path);
}

/* ---- fdopen/freopen ---- упрощённо ---- */
FILE* fdopen(int fd, const char* mode) {
    /* Оборачиваем существующий fd в FILE*. Выделяем структуру. */
    extern void* __libc_malloc_impl(size_t);
    if (fd < 0) return NULL;
    struct _FILE* f = (struct _FILE*)__libc_malloc_impl(sizeof(struct _FILE));
    if (!f) return NULL;
    f->fd = fd;
    f->eof = 0; f->error = 0;
    f->mode = (mode && mode[0] == 'w') ? 'w' : (mode && mode[0]=='a' ? 'a' : 'r');
    f->buf = f->tinybuf;
    f->buf_pos = 0; f->buf_size = 0; f->buf_cap = BUFSIZ;
    return f;
}

FILE* freopen(const char* path, const char* mode, FILE* stream) {
    if (stream && stream->fd >= 0) {
        __libc_close_impl(stream->fd);
        stream->fd = -1;
    }
    FILE* nf = fopen(path, mode);
    if (!nf) return NULL;
    if (stream) {
        *stream = *nf;
        /* nf утечёт, но freopen редко используется */
        return stream;
    }
    return nf;
}

/* ---- mkstemp: создаёт уникальный временный файл ----
   Шаблон оканчивается на XXXXXX, заменяем на счётчик. */
int mkstemp(char* template) {
    extern int __libc_open_impl(const char*, int);
    static unsigned counter = 0;
    size_t len = 0; while (template[len]) len++;
    if (len < 6) return -1;
    char* x = template + len - 6;
    for (int attempt = 0; attempt < 1000; attempt++) {
        unsigned v = counter++ + attempt;
        for (int i = 0; i < 6; i++) { x[i] = 'a' + (v % 26); v /= 26; }
        /* O_CREAT|O_RDWR|O_EXCL = 0x40|0x2|0x80 */
        int fd = __libc_open_impl(template, 0x40 | 0x2 | 0x80);
        if (fd >= 0) return fd;
    }
    return -1;
}

/* ---- open_memstream: пишущий поток в память ----
   chibicc использует для сборки строк. Упрощённо: динамический буфер. */
struct memstream { char** bufp; size_t* sizep; char* buf; size_t size; size_t cap; };

FILE* open_memstream(char** bufp, size_t* sizep) {
    extern void* __libc_malloc_impl(size_t);
    struct _FILE* f = (struct _FILE*)__libc_malloc_impl(sizeof(struct _FILE));
    if (!f) return 0;
    for (size_t i = 0; i < sizeof(struct _FILE); i++) ((char*)f)[i] = 0;
    f->fd = -1;
    f->mode = 'm';
    f->mem_bufp = bufp;
    f->mem_sizep = sizep;
    f->mem_cap = 256;
    f->mem_buf = (char*)__libc_malloc_impl(f->mem_cap);
    f->mem_len = 0;
    if (f->mem_buf) { f->mem_buf[0] = '\0'; }
    if (bufp) *bufp = f->mem_buf;
    if (sizep) *sizep = 0;
    return f;
}
