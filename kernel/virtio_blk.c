/*
 * virtio_blk.c — драйвер virtio block device (legacy, PCI port I/O).
 *
 * Legacy virtio (device 0x1001) использует BAR0 = I/O port base.
 * Регистры (offset от BAR0):
 *   0x00  DEVICE_FEATURES   (r)
 *   0x04  GUEST_FEATURES    (w)
 *   0x08  QUEUE_ADDR        (w) — физ. адрес virtqueue >> 12
 *   0x0C  QUEUE_SIZE        (r)
 *   0x0E  QUEUE_SELECT      (w)
 *   0x10  QUEUE_NOTIFY      (w)
 *   0x12  DEVICE_STATUS     (rw)
 *   0x13  ISR_STATUS        (r)
 *   0x14  config...         (device-specific: capacity и т.д.)
 *
 * Virtqueue layout (split):
 *   descriptors[QSIZE]  — каждый 16 байт
 *   avail ring          — flags(2) idx(2) ring[QSIZE](2) used_event(2)
 *   (padding до 4096)
 *   used ring           — flags(2) idx(2) ring[QSIZE](8) avail_event(2)
 *
 * Для блочного запроса нужны 3 дескриптора:
 *   1) header (тип, sector) — read-only для устройства
 *   2) data buffer          — write (для read) или read (для write)
 *   3) status byte          — write-only устройством
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include "io.h"
#include "pci.h"
#include "pmm.h"
#include "virtio_blk.h"

/* Регистры (offset от I/O base) */
#define VIRTIO_DEVICE_FEATURES  0x00
#define VIRTIO_GUEST_FEATURES   0x04
#define VIRTIO_QUEUE_ADDR       0x08
#define VIRTIO_QUEUE_SIZE       0x0C
#define VIRTIO_QUEUE_SELECT     0x0E
#define VIRTIO_QUEUE_NOTIFY     0x10
#define VIRTIO_DEVICE_STATUS    0x12
#define VIRTIO_ISR_STATUS       0x13

/* Status флаги */
#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEAT_OK   8

/* Типы блочных запросов */
#define VIRTIO_BLK_T_IN   0   /* read */
#define VIRTIO_BLK_T_OUT  1   /* write */

/* Дескриптор virtqueue */
typedef struct {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} __attribute__((packed)) virtq_desc_t;

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

typedef struct {
    u16 flags;
    u16 idx;
    u16 ring[];      /* QSIZE */
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    u32 id;
    u32 len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    u16 flags;
    u16 idx;
    virtq_used_elem_t ring[];
} __attribute__((packed)) virtq_used_t;

/* Заголовок блочного запроса */
typedef struct {
    u32 type;
    u32 reserved;
    u64 sector;
} __attribute__((packed)) virtio_blk_req_hdr_t;

/* Состояние устройства */
static u16 io_base = 0;
static int qsize = 0;
static virtq_desc_t*  desc  = NULL;
static virtq_avail_t* avail = NULL;
static virtq_used_t*  used  = NULL;
static u16 last_used_idx = 0;
static int initialized = 0;

/* Буферы запроса в выделенной странице. */
static virtio_blk_req_hdr_t* req_hdr = NULL;
static volatile u8* req_status = NULL;

static inline u8  vio_r8 (u16 off) { return inb(io_base + off); }
static inline u16 vio_r16(u16 off) { return inw(io_base + off); }
static inline u32 vio_r32(u16 off) { return inl(io_base + off); }
static inline void vio_w8 (u16 off, u8 v)  { outb(io_base + off, v); }
static inline void vio_w16(u16 off, u16 v) { outw(io_base + off, v); }
static inline void vio_w32(u16 off, u32 v) { outl(io_base + off, v); }

int virtio_blk_init(void) {
    pci_device_t* dev = pci_find_by_id(0x1af4, 0x1001);
    if (!dev) {
        printf("[virtio-blk] device not found\n");
        return -1;
    }

    /* BAR0 — I/O порт (бит 0 = 1). Маскируем младшие биты. */
    u32 bar0 = dev->bar[0];
    if (!(bar0 & 1)) {
        printf("[virtio-blk] BAR0 не I/O (memory-mapped не поддержан)\n");
        return -1;
    }
    io_base = (u16)(bar0 & ~0x3);
    printf("[virtio-blk] found at %02x:%02x.%x, io_base=0x%x\n",
           dev->bus, dev->dev, dev->func, io_base);

    /* Reset: status = 0 */
    vio_w8(VIRTIO_DEVICE_STATUS, 0);
    /* ACK + DRIVER */
    vio_w8(VIRTIO_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    vio_w8(VIRTIO_DEVICE_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Features: для простоты принимаем 0 (никаких расширений). */
    u32 features = vio_r32(VIRTIO_DEVICE_FEATURES);
    (void)features;
    vio_w32(VIRTIO_GUEST_FEATURES, 0);
    vio_w8(VIRTIO_DEVICE_STATUS,
           VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEAT_OK);

    /* Настройка queue 0 */
    vio_w16(VIRTIO_QUEUE_SELECT, 0);
    qsize = vio_r16(VIRTIO_QUEUE_SIZE);
    if (qsize == 0) {
        printf("[virtio-blk] queue 0 size = 0\n");
        return -1;
    }
    printf("[virtio-blk] queue size = %d\n", qsize);

    /*
     * Legacy virtqueue layout с выравниванием (qalign=4096):
     *   desc[qsize]          : qsize * 16 байт
     *   avail               : 6 + 2*qsize байт, сразу после desc
     *   --- padding до 4096 ---
     *   used                : 6 + 8*qsize байт, на следующей 4096-границе
     *
     * Размер desc для qsize=256 = 4096 (ровно страница), avail на стр.2,
     * used на стр.3 (align). Выделим 3 непрерывные страницы.
     *
     * PMM выдаёт страницы по убыванию/возрастанию — нам нужны 3 ПОДРЯД.
     * Аллоцируем и проверяем непрерывность; если нет — повторяем.
     */
    void* qmem = NULL;
    void* pages[3];
    for (int attempt = 0; attempt < 64; attempt++) {
        pages[0] = pmm_alloc_page();
        pages[1] = pmm_alloc_page();
        pages[2] = pmm_alloc_page();
        if (!pages[0] || !pages[1] || !pages[2]) {
            printf("[virtio-blk] oom virtqueue\n"); return -1;
        }
        if ((u8*)pages[1] == (u8*)pages[0] + 4096 &&
            (u8*)pages[2] == (u8*)pages[0] + 8192) {
            qmem = pages[0];
            break;
        }
        /* не подряд — освобождаем и пробуем снова (PMM free list) */
        pmm_free_page(pages[0]);
        pmm_free_page(pages[1]);
        pmm_free_page(pages[2]);
    }
    if (!qmem) { printf("[virtio-blk] не удалось выделить 3 подряд страницы\n"); return -1; }
    memset(qmem, 0, 4096 * 3);

    desc  = (virtq_desc_t*)qmem;
    avail = (virtq_avail_t*)((u8*)qmem + qsize * sizeof(virtq_desc_t));
    used  = (virtq_used_t*)((u8*)qmem + 8192);   /* третья страница */

    u64 pfn = (u64)(uintptr_t)qmem >> 12;
    vio_w32(VIRTIO_QUEUE_ADDR, (u32)pfn);

    last_used_idx = 0;

    /* Выделяем страницу для req_hdr + req_status */
    void* reqp = pmm_alloc_page();
    if (!reqp) { printf("[virtio-blk] oom req\n"); return -1; }
    memset(reqp, 0, 4096);
    req_hdr = (virtio_blk_req_hdr_t*)reqp;
    req_status = (volatile u8*)((u8*)reqp + 64);

    /* DRIVER_OK */
    vio_w8(VIRTIO_DEVICE_STATUS,
           VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEAT_OK | VIRTIO_STATUS_DRIVER_OK);

    initialized = 1;
    printf("[virtio-blk] initialized OK\n");
    return 0;
}

/*
 * Чтение/запись одного сектора (512 байт).
 * write=0 → read, write=1 → write.
 */
static int virtio_blk_rw(u64 sector, void* buf, int write) {
    if (!initialized) return -1;

    req_hdr->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    req_hdr->reserved = 0;
    req_hdr->sector = sector;
    *req_status = 0xFF;
    __asm__ volatile("" ::: "memory");

    /* 3 дескриптора: header, data, status */
    desc[0].addr  = (u64)(uintptr_t)req_hdr;
    desc[0].len   = sizeof(*req_hdr);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next  = 1;

    desc[1].addr  = (u64)(uintptr_t)buf;
    desc[1].len   = 512;
    desc[1].flags = VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE);
    desc[1].next  = 2;

    desc[2].addr  = (u64)(uintptr_t)req_status;
    desc[2].len   = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next  = 0;

    /* Кладём head дескриптора (0) в avail ring */
    avail->ring[avail->idx % qsize] = 0;
    __asm__ volatile("" ::: "memory");
    avail->idx++;
    __asm__ volatile("" ::: "memory");

    /* Notify устройство */
    vio_w16(VIRTIO_QUEUE_NOTIFY, 0);

    /* Polling: ждём пока used->idx продвинется */
    int timeout = 100000000;
    u16 target = last_used_idx + 1;
    while (used->idx != target) {
        if (--timeout <= 0) {
            printf("[virtio-blk] timeout: used->idx=%d target=%d status=%d\n",
                   used->idx, target, req_status);
            return -1;
        }
        __asm__ volatile("pause");
    }
    last_used_idx = used->idx;

    __asm__ volatile("" ::: "memory");
    u8 st = *req_status;
    if (st != 0) {
        u16 slot = (last_used_idx - 1) % qsize;
        printf("[virtio-blk] req(w=%d sec=%lu) status=%d used.idx=%d ring[%d].id=%d len=%d\n",
               write, sector, st, used->idx,
               slot, used->ring[slot].id, used->ring[slot].len);
        return -1;
    }
    return 0;
}

int virtio_blk_read(u64 sector, void* buf) {
    return virtio_blk_rw(sector, buf, 0);
}

int virtio_blk_write(u64 sector, void* buf) {
    return virtio_blk_rw(sector, buf, 1);
}

int virtio_blk_available(void) {
    return initialized;
}
