/*
 * mouse.c — драйвер PS/2 мыши (IRQ12). 3-байтные пакеты:
 * [flags][dx][dy]. Отслеживает позицию и кнопки.
 */
#include "types.h"
#include <stdio.h>
#include "idt.h"
#include "pic.h"
#include "mouse.h"

static inline u8 inb(u16 p){u8 v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
static inline void outb(u16 p,u8 v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}

static void wait_write(void){ int t=100000; while(t-- && (inb(0x64)&2)); }
static void wait_read(void){ int t=100000; while(t-- && !(inb(0x64)&1)); }

static void mouse_cmd(u8 cmd){
    wait_write(); outb(0x64, 0xD4);   /* команда мыши */
    wait_write(); outb(0x60, cmd);
    wait_read();  inb(0x60);           /* ACK */
}

/* Состояние мыши */
static volatile int mouse_x = 512, mouse_y = 384;
static volatile u8  mouse_buttons = 0;
static int max_x = 1024, max_y = 768;

static u8 packet[3];
static int packet_idx = 0;
volatile unsigned mouse_irq_count = 0;

static void mouse_irq(u8 irq){
    mouse_irq_count++;
    u8 status = inb(0x64);
    if (!(status & 0x20)) { return; }  /* не от мыши */
    u8 data = inb(0x60);

    switch (packet_idx) {
    case 0:
        if (!(data & 0x08)) { return; }  /* sync bit */
        packet[0] = data; packet_idx = 1; break;
    case 1:
        packet[1] = data; packet_idx = 2; break;
    case 2:
        packet[2] = data; packet_idx = 0;
        {
            u8 flags = packet[0];
            int dx = (int)packet[1] - ((flags & 0x10) ? 256 : 0);
            int dy = (int)packet[2] - ((flags & 0x20) ? 256 : 0);
            mouse_x += dx;
            mouse_y -= dy;   /* экранный Y инвертирован */
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x >= max_x) mouse_x = max_x - 1;
            if (mouse_y >= max_y) mouse_y = max_y - 1;
            mouse_buttons = flags & 0x07;
        }
        break;
    }
}

void mouse_init(int screen_w, int screen_h){
    max_x = screen_w; max_y = screen_h;
    mouse_x = screen_w/2; mouse_y = screen_h/2;

    /* Включаем вспомогательное устройство (мышь) */
    wait_write(); outb(0x64, 0xA8);

    /* Сброс мыши (0xFF) — ждём self-test 0xAA + id 0x00 */
    mouse_cmd(0xFF);
    for (volatile int i=0;i<100000;i++);
    /* проглотим self-test ответы если есть */
    while (inb(0x64) & 1) { inb(0x60); for(volatile int i=0;i<1000;i++); }

    /* Включаем IRQ12 в командном байте контроллера */
    wait_write(); outb(0x64, 0x20);
    wait_read();  u8 cb = inb(0x60);
    cb |= 2;        /* бит 1: IRQ12 enable (мышь) */
    cb |= 1;        /* бит 0: IRQ1 enable (клава) */
    cb &= ~0x20;    /* бит 5: clock enable для мыши */
    wait_write(); outb(0x64, 0x60);
    wait_write(); outb(0x60, cb);

    mouse_cmd(0xF6);   /* set defaults */
    mouse_cmd(0xF3);   /* set sample rate */
    mouse_cmd(0x64);   /*   100 пакетов/сек */
    mouse_cmd(0xF4);   /* enable packet streaming */

    irq_register(12, mouse_irq);
    pic_clear_mask(2);   /* каскад для второго PIC (СНАЧАЛА) */
    pic_clear_mask(12);
    {
        u8 m1 = inb(0x21), m2 = inb(0xA1);
        printf("[mouse] PS/2 готова (IRQ12), PIC mask m1=0x%x m2=0x%x\n", m1, m2);
    }
}

void mouse_get(int* x, int* y, unsigned* buttons){
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons) *buttons = mouse_buttons;
}
