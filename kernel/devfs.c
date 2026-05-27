/*
 * devfs.c — крошечная файловая система для устройств.
 *
 * Не настоящая FS — просто статическая таблица vnode'ов. devfs
 * подключается отдельным mount-point'ом ("/dev"), но пока у нас
 * один корень, повесим устройства прямо в корне initrd-FS
 * (см. main.c).
 *
 * Устройства:
 *   /dev/null    — read возвращает 0, write проглатывает всё
 *   /dev/zero    — read заполняет нулями, write проглатывает
 *   /dev/console — read EOF (нет клавы пока), write → VGA+serial
 */

#include "types.h"
#include <string.h>
#include "vfs.h"
#include "devfs.h"

extern void kputs_raw(const char* s, size_t n);

/* ---------- /dev/null ---------- */
static ssize_t null_read(vnode_t* v, void* buf, size_t n, off_t off) {
    (void)v; (void)buf; (void)n; (void)off;
    return 0;       /* всегда EOF */
}
static ssize_t null_write(vnode_t* v, const void* buf, size_t n, off_t off) {
    (void)v; (void)buf; (void)off;
    return (ssize_t)n;  /* проглатываем */
}
static const struct vnode_ops null_ops = {
    .read = null_read, .write = null_write, .lookup = NULL, .readdir = NULL,
};

/* ---------- /dev/zero ---------- */
static ssize_t zero_read(vnode_t* v, void* buf, size_t n, off_t off) {
    (void)v; (void)off;
    memset(buf, 0, n);
    return (ssize_t)n;
}
static const struct vnode_ops zero_ops = {
    .read = zero_read, .write = null_write, .lookup = NULL, .readdir = NULL,
};

/* ---------- /dev/console ---------- */
static ssize_t console_read(vnode_t* v, void* buf, size_t n, off_t off) {
    (void)v; (void)buf; (void)n; (void)off;
    return 0;       /* пока нет клавиатуры */
}
static ssize_t console_write(vnode_t* v, const void* buf, size_t n, off_t off) {
    (void)v; (void)off;
    kputs_raw((const char*)buf, n);
    return (ssize_t)n;
}
static const struct vnode_ops console_ops = {
    .read = console_read, .write = console_write, .lookup = NULL, .readdir = NULL,
};

/* ---------- /dev/fb0 — framebuffer ---------- */
static off_t fb_dev_pos = 0;
static ssize_t fb_write(vnode_t* v, const void* buf, size_t n, off_t off) {
    (void)v;
    extern int fb_is_ready(void);
    extern u32 fb_get_width(void);
    extern u32 fb_get_height(void);
    extern void fb_putpixel(u32, u32, u32);
    if (!fb_is_ready()) return (ssize_t)n;
    u32 w = fb_get_width(), h = fb_get_height();
    /* off игнорируем для потоковой записи — используем внутр. позицию.
       Cairo пишет построчно W*4 байта; трактуем буфер как пиксели ARGB. */
    off_t pos = (off > 0) ? off : fb_dev_pos;
    const u32* px = (const u32*)buf;
    size_t npx = n / 4;
    for (size_t i = 0; i < npx; i++) {
        u32 idx = (u32)(pos / 4) + i;
        u32 x = idx % w, y = idx / w;
        if (y < h) fb_putpixel(x, y, px[i] & 0x00FFFFFF);
    }
    fb_dev_pos = pos + (off_t)n;
    if ((u32)(fb_dev_pos / 4) >= w * h) fb_dev_pos = 0;  /* wrap кадра */
    return (ssize_t)n;
}
static ssize_t fb_read(vnode_t* v, void* buf, size_t n, off_t off) {
    (void)v; (void)off; memset(buf, 0, n); return (ssize_t)n;
}
static const struct vnode_ops fb_ops = {
    .read = fb_read, .write = fb_write, .lookup = NULL, .readdir = NULL,
};

/* ---------- /dev/input — состояние мыши и клавиатуры ----------
   read возвращает 16 байт: int32 x, y, buttons, key (0 если нет). */
static ssize_t input_read(vnode_t* v, void* buf, size_t n, off_t off) {
    (void)v; (void)off;
    extern void mouse_get(int*, int*, unsigned*);
    extern int  keyboard_getchar(void);
    if (n < 16) return 0;
    int* out = (int*)buf;
    int x=0, y=0; unsigned b=0;
    mouse_get(&x, &y, &b);
    extern volatile unsigned mouse_irq_count;
    out[0] = x; out[1] = y; out[2] = (int)b;
    extern volatile unsigned kbd_irq_count;
    out[3] = (int)kbd_irq_count;   /* отладка: число IRQ1 (клава) */
    return 16;
}
static ssize_t input_write(vnode_t* v, const void* buf, size_t n, off_t off) {
    (void)v; (void)buf; (void)off; return (ssize_t)n;
}
static const struct vnode_ops input_ops = {
    .read = input_read, .write = input_write, .lookup = NULL, .readdir = NULL,
};

/* ---------- /dev/fbinfo — параметры фреймбуфера ---------- */
/* read возвращает 16 байт: u32 width, u32 height, u32 pitch, u32 bpp */
static ssize_t fbinfo_read(vnode_t* v, void* buf, size_t n, off_t off) {
    (void)v; (void)off;
    extern u32 fb_get_width(void); extern u32 fb_get_height(void);
    extern u32 fb_get_pitch(void); extern u32 fb_get_bpp(void);
    if (n < 16) return 0;
    u32* o = (u32*)buf;
    o[0] = fb_get_width(); o[1] = fb_get_height();
    o[2] = fb_get_pitch(); o[3] = fb_get_bpp();
    return 16;
}
static const struct vnode_ops fbinfo_ops = {
    .read = fbinfo_read, .write = NULL, .lookup = NULL, .readdir = NULL,
};

/* Статические vnode-объекты */
static vnode_t dev_null    = { .type = VNODE_CHAR, .size = 0, .ops = &null_ops };
static vnode_t dev_zero    = { .type = VNODE_CHAR, .size = 0, .ops = &zero_ops };
static vnode_t dev_console = { .type = VNODE_CHAR, .size = 0, .ops = &console_ops };
static vnode_t dev_fb0     = { .type = VNODE_CHAR, .size = 0, .ops = &fb_ops };
static vnode_t dev_input   = { .type = VNODE_CHAR, .size = 0, .ops = &input_ops };
static vnode_t dev_fbinfo  = { .type = VNODE_CHAR, .size = 0, .ops = &fbinfo_ops };

vnode_t* devfs_get(const char* name) {
    if (!strcmp(name, "null"))    return &dev_null;
    if (!strcmp(name, "zero"))    return &dev_zero;
    if (!strcmp(name, "console")) return &dev_console;
    if (!strcmp(name, "fb0"))     return &dev_fb0;
    if (!strcmp(name, "input"))   return &dev_input;
    if (!strcmp(name, "fbinfo"))  return &dev_fbinfo;
    return NULL;
}

vnode_t* devfs_console(void) { return &dev_console; }
