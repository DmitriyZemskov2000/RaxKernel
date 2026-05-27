/*
 * pipe.c — анонимные pipe'ы между процессами.
 *
 * Каждый pipe = ring buffer + два vnode'а (read end и write end).
 * read из пустого pipe блокирует пока кто-то не запишет.
 * write в полный pipe блокирует пока кто-то не прочитает.
 *
 * Для GCC pipeline (cpp | cc1 | as | ld) нужен dup2 чтобы
 * перенаправить stdout child'а в pipe.
 */

#include "types.h"
#include <string.h>
#include <stdlib.h>
#include "vfs.h"
#include "sched.h"

#define PIPE_BUF_SIZE 4096
#define MAX_PIPES 16

typedef struct {
    int  in_use;
    char buf[PIPE_BUF_SIZE];
    size_t head;       /* куда писать */
    size_t tail;       /* откуда читать */
    size_t count;      /* сколько байт в буфере */
    int  read_closed;
    int  write_closed;
    int  refs_read;    /* сколько FD'ов держат read end */
    int  refs_write;
} pipe_t;

static pipe_t pipes[MAX_PIPES];

extern void* kmalloc(size_t);
extern void  kfree(void*);

/* Внутри vnode->priv будет указатель на pipe + флаг "это read end" */
typedef struct {
    pipe_t* pipe;
    int     is_read_end;
} pipe_priv_t;

static ssize_t pipe_read(vnode_t* v, void* buf, size_t n, off_t off) {
    (void)off;
    pipe_priv_t* pp = (pipe_priv_t*)v->priv;
    pipe_t* p = pp->pipe;
    if (!pp->is_read_end) return -9;

    while (p->count == 0) {
        if (p->write_closed || p->refs_write == 0) return 0;   /* EOF */
        extern long sys_yield(void);
        sys_yield();
    }
    size_t cnt = (n < p->count) ? n : p->count;
    for (size_t i = 0; i < cnt; i++) {
        ((char*)buf)[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    p->count -= cnt;
    return (ssize_t)cnt;
}

static ssize_t pipe_write(vnode_t* v, const void* buf, size_t n, off_t off) {
    (void)off;
    pipe_priv_t* pp = (pipe_priv_t*)v->priv;
    pipe_t* p = pp->pipe;
    if (pp->is_read_end) return -9;
    if (p->read_closed || p->refs_read == 0) return -32;  /* -EPIPE */

    size_t written = 0;
    while (written < n) {
        while (p->count == PIPE_BUF_SIZE) {
            if (p->read_closed || p->refs_read == 0) return -32;
            extern long sys_yield(void);
            sys_yield();
        }
        size_t avail = PIPE_BUF_SIZE - p->count;
        size_t need = n - written;
        size_t cnt = (avail < need) ? avail : need;
        for (size_t i = 0; i < cnt; i++) {
            p->buf[p->head] = ((const char*)buf)[written + i];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
        }
        p->count += cnt;
        written += cnt;
    }
    return (ssize_t)written;
}

static const struct vnode_ops pipe_read_ops = {
    .read = pipe_read, .write = NULL, .lookup = NULL, .readdir = NULL,
};
static const struct vnode_ops pipe_write_ops = {
    .read = NULL, .write = pipe_write, .lookup = NULL, .readdir = NULL,
};

/* Создание pipe — возвращает 2 vnode'а */
int pipe_create(vnode_t** out_read, vnode_t** out_write) {
    pipe_t* p = NULL;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].in_use) { p = &pipes[i]; break; }
    }
    if (!p) return -1;
    memset(p, 0, sizeof(*p));
    p->in_use = 1;
    p->refs_read = 1;
    p->refs_write = 1;

    vnode_t* rv = (vnode_t*)kmalloc(sizeof(vnode_t));
    vnode_t* wv = (vnode_t*)kmalloc(sizeof(vnode_t));
    pipe_priv_t* rp = (pipe_priv_t*)kmalloc(sizeof(pipe_priv_t));
    pipe_priv_t* wp = (pipe_priv_t*)kmalloc(sizeof(pipe_priv_t));
    if (!rv || !wv || !rp || !wp) {
        if (rv) kfree(rv); if (wv) kfree(wv);
        if (rp) kfree(rp); if (wp) kfree(wp);
        p->in_use = 0;
        return -1;
    }

    rp->pipe = p; rp->is_read_end = 1;
    wp->pipe = p; wp->is_read_end = 0;

    rv->type = VNODE_CHAR;
    rv->size = 0;
    rv->ops = &pipe_read_ops;
    rv->priv = rp;

    wv->type = VNODE_CHAR;
    wv->size = 0;
    wv->ops = &pipe_write_ops;
    wv->priv = wp;

    *out_read = rv;
    *out_write = wv;
    return 0;
}
