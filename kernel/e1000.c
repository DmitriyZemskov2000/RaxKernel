/*
 * e1000.c — драйвер сетевой карты Intel 82540EM (8086:100e), которую
 * эмулирует QEMU. Достаточно для приёма/передачи Ethernet-кадров.
 *
 * Архитектура: MMIO-регистры (BAR0), кольца дескрипторов RX и TX в
 * физической памяти, DMA. Прерывания пока не используем — поллинг.
 */
#include "types.h"
#include <string.h>
#include <stdio.h>
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "e1000.h"

/* ---- Регистры e1000 (смещения в MMIO) ---- */
#define E1000_CTRL    0x0000   /* Device Control */
#define E1000_STATUS  0x0008   /* Device Status */
#define E1000_EERD    0x0014   /* EEPROM Read */
#define E1000_ICR     0x00C0   /* Interrupt Cause Read */
#define E1000_IMS     0x00D0   /* Interrupt Mask Set */
#define E1000_IMC     0x00D8   /* Interrupt Mask Clear */
#define E1000_RCTL    0x0100   /* Receive Control */
#define E1000_TCTL    0x0400   /* Transmit Control */
#define E1000_TIPG    0x0410   /* Transmit IPG */
#define E1000_RDBAL   0x2800   /* RX Descriptor Base Low */
#define E1000_RDBAH   0x2804   /* RX Descriptor Base High */
#define E1000_RDLEN   0x2808   /* RX Descriptor Length */
#define E1000_RDH     0x2810   /* RX Descriptor Head */
#define E1000_RDT     0x2818   /* RX Descriptor Tail */
#define E1000_TDBAL   0x3800
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_RAL     0x5400   /* Receive Address Low (MAC) */
#define E1000_RAH     0x5404   /* Receive Address High */

/* RCTL биты */
#define RCTL_EN       (1 << 1)
#define RCTL_BAM      (1 << 15)  /* broadcast accept */
#define RCTL_SECRC    (1 << 26)  /* strip CRC */
#define RCTL_BSIZE_2048 (0 << 16)

/* TCTL биты */
#define TCTL_EN       (1 << 1)
#define TCTL_PSP      (1 << 3)   /* pad short packets */

/* Дескриптор статусы */
#define RX_STATUS_DD  (1 << 0)   /* descriptor done */
#define RX_STATUS_EOP (1 << 1)   /* end of packet */
#define TX_CMD_EOP    (1 << 0)
#define TX_CMD_IFCS   (1 << 1)   /* insert FCS */
#define TX_CMD_RS     (1 << 3)   /* report status */
#define TX_STATUS_DD  (1 << 0)

#define NUM_RX_DESC 32
#define NUM_TX_DESC 8
#define RX_BUF_SIZE 2048

typedef struct PACKED {
    u64 addr;
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
} e1000_rx_desc_t;

typedef struct PACKED {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} e1000_tx_desc_t;

static volatile u8* mmio = 0;
static u8 mac[6];

static e1000_rx_desc_t* rx_descs;
static e1000_tx_desc_t* tx_descs;
static u8* rx_bufs[NUM_RX_DESC];
static u8* tx_bufs[NUM_TX_DESC];
static u32 rx_cur = 0;
static u32 tx_cur = 0;
static int e1000_ready = 0;

static inline void mmio_w32(u32 off, u32 val) {
    *(volatile u32*)(mmio + off) = val;
}
static inline u32 mmio_r32(u32 off) {
    return *(volatile u32*)(mmio + off);
}

/* Чтение MAC из EEPROM или из RAL/RAH */
static void read_mac(void) {
    u32 ral = mmio_r32(E1000_RAL);
    u32 rah = mmio_r32(E1000_RAH);
    if (ral != 0) {
        mac[0] = ral & 0xFF;
        mac[1] = (ral >> 8) & 0xFF;
        mac[2] = (ral >> 16) & 0xFF;
        mac[3] = (ral >> 24) & 0xFF;
        mac[4] = rah & 0xFF;
        mac[5] = (rah >> 8) & 0xFF;
        return;
    }
    /* Иначе через EEPROM */
    for (int i = 0; i < 3; i++) {
        mmio_w32(E1000_EERD, (i << 8) | 1);
        u32 v;
        while (!((v = mmio_r32(E1000_EERD)) & (1 << 4))) {}
        u16 w = (v >> 16) & 0xFFFF;
        mac[i*2]   = w & 0xFF;
        mac[i*2+1] = (w >> 8) & 0xFF;
    }
}

/* phys-адрес для DMA: у нас kernel identity-mapped в нижней памяти,
   поэтому виртуальный адрес из kmalloc страниц = физический (для
   pmm_alloc_page). Используем pmm_alloc_page (возвращает физ. = идентич). */
static void* alloc_dma_page(void) {
    void* p = pmm_alloc_page();
    if (p) memset(p, 0, 4096);
    return p;
}

static void rx_init(void) {
    void* page = alloc_dma_page();   /* хватит на 32 дескриптора (16 байт каждый = 512) */
    rx_descs = (e1000_rx_desc_t*)page;
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_bufs[i] = (u8*)alloc_dma_page();   /* 4096, используем 2048 */
        rx_descs[i].addr = (u64)rx_bufs[i];
        rx_descs[i].status = 0;
    }
    u64 base = (u64)rx_descs;
    mmio_w32(E1000_RDBAL, (u32)(base & 0xFFFFFFFF));
    mmio_w32(E1000_RDBAH, (u32)(base >> 32));
    mmio_w32(E1000_RDLEN, NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    mmio_w32(E1000_RDH, 0);
    mmio_w32(E1000_RDT, NUM_RX_DESC - 1);
    mmio_w32(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048
             | (1 << 3) | (1 << 4));  /* UPE | MPE: promiscuous unicast+multicast */
    rx_cur = 0;
}

static void tx_init(void) {
    void* page = alloc_dma_page();
    tx_descs = (e1000_tx_desc_t*)page;
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_bufs[i] = (u8*)alloc_dma_page();
        tx_descs[i].addr = (u64)tx_bufs[i];
        tx_descs[i].status = TX_STATUS_DD;  /* помечаем свободными */
        tx_descs[i].cmd = 0;
    }
    u64 base = (u64)tx_descs;
    mmio_w32(E1000_TDBAL, (u32)(base & 0xFFFFFFFF));
    mmio_w32(E1000_TDBAH, (u32)(base >> 32));
    mmio_w32(E1000_TDLEN, NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    mmio_w32(E1000_TDH, 0);
    mmio_w32(E1000_TDT, 0);
    mmio_w32(E1000_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));
    mmio_w32(E1000_TIPG, 0x0060200A);
    tx_cur = 0;
}

int e1000_init(void) {
    pci_device_t* dev = pci_find_by_id(0x8086, 0x100e);
    if (!dev) {
        printf("[e1000] карта 8086:100e не найдена\n");
        return -1;
    }

    /* BAR0 — MMIO base (маскируем младшие биты типа/флагов) */
    u64 bar0 = dev->bar[0] & ~0xFULL;

    /* Включаем bus mastering и memory space в PCI command регистре (off 0x04) */
    u32 cmd = pci_read32(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= (1 << 2) | (1 << 1);   /* bus master + memory space */
    pci_write32(dev->bus, dev->dev, dev->func, 0x04, cmd);

    /* Маппим MMIO регион (128 KiB) в kernel space */
    u64 vbase = 0xFFFFFFFFB0000000ULL;
    for (int i = 0; i < 32; i++) {   /* 32 * 4KiB = 128 KiB */
        vmm_map(vbase + i * 4096, bar0 + i * 4096, VMM_WRITABLE);
    }
    mmio = (volatile u8*)vbase;

    read_mac();
    printf("[e1000] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Сброс прерываний */
    mmio_w32(E1000_IMC, 0xFFFFFFFF);

    rx_init();
    tx_init();

    e1000_ready = 1;
    printf("[e1000] инициализирована (RX %d, TX %d дескрипторов)\n",
           NUM_RX_DESC, NUM_TX_DESC);
    /* Диагностика: STATUS регистр (link up?), проверка MMIO. */
    {
        u32 status = mmio_r32(E1000_STATUS);
        printf("[e1000] STATUS=0x%x (link %s, TCTL=0x%x)\n",
               status, (status & (1<<1)) ? "UP" : "down",
               mmio_r32(E1000_TCTL));
    }
    return 0;
}

int e1000_is_ready(void) { return e1000_ready; }

void e1000_get_mac(u8* out) { memcpy(out, mac, 6); }

/* Отправка кадра (data, len). Возвращает 0 при успехе. */
int e1000_send(const void* data, u16 len) {
    if (!e1000_ready || len > RX_BUF_SIZE) return -1;
    u32 i = tx_cur;
    e1000_tx_desc_t* d = &tx_descs[i];
    memcpy(tx_bufs[i], data, len);
    d->addr = (u64)tx_bufs[i];
    d->length = len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;

    tx_cur = (i + 1) % NUM_TX_DESC;
    mmio_w32(E1000_TDT, tx_cur);

    /* поллим завершение */
    int spin = 2000000;
    while (!(d->status & TX_STATUS_DD) && spin-- > 0) {
        __asm__ volatile("pause");
    }
    return (d->status & TX_STATUS_DD) ? 0 : -2;
}

/* Приём кадра. Возвращает длину или 0 если нет пакета. */
int e1000_receive(void* buf, u16 maxlen) {
    if (!e1000_ready) return 0;
    e1000_rx_desc_t* d = &rx_descs[rx_cur];
    if (!(d->status & RX_STATUS_DD)) return 0;   /* нет пакета */

    u16 len = d->length;
    if (len > maxlen) len = maxlen;
    memcpy(buf, rx_bufs[rx_cur], len);

    d->status = 0;
    u32 old = rx_cur;
    rx_cur = (rx_cur + 1) % NUM_RX_DESC;
    mmio_w32(E1000_RDT, old);   /* возвращаем дескриптор карте */
    return len;
}

u32 e1000_dbg_rdh(void) { return mmio_r32(E1000_RDH); }
u32 e1000_dbg_rdt(void) { return mmio_r32(E1000_RDT); }
