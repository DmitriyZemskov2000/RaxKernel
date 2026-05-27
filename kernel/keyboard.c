/*
 * keyboard.c — драйвер PS/2 клавиатуры (IRQ1, порт 0x60).
 * Scancode set 1 -> ASCII, кольцевой буфер ввода.
 */
#include "types.h"
#include <stdio.h>
#include "idt.h"
#include "pic.h"
#include "keyboard.h"

static inline u8 inb(u16 port) {
    u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Scancode set 1 -> ASCII (US), без модификаторов */
static const char sc_normal[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ',
};
static const char sc_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ',
};

#define KBD_BUF_SIZE 256
static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile u32 kbd_head = 0, kbd_tail = 0;

static int shift_down = 0;
static int ctrl_down = 0;

static void kbd_push(char c) {
    u32 next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buf[kbd_head] = c;
        kbd_head = next;
    }
}

/* Обработчик IRQ1 */
volatile unsigned kbd_irq_count=0;
static void keyboard_irq(u8 irq) { kbd_irq_count++;
    (void)irq;
    u8 sc = inb(0x60);

    if (sc & 0x80) {
        /* отпускание клавиши */
        u8 code = sc & 0x7F;
        if (code == 0x2A || code == 0x36) shift_down = 0;
        else if (code == 0x1D) ctrl_down = 0;
    } else {
        if (sc == 0x2A || sc == 0x36) { shift_down = 1; }
        else if (sc == 0x1D) { ctrl_down = 1; }
        else if (sc < 128) {
            char c = shift_down ? sc_shift[sc] : sc_normal[sc];
            if (c) kbd_push(c);
        }
    }
}

void keyboard_init(void) {
    /* очистим выходной буфер контроллера */
    while (inb(0x64) & 1) inb(0x60);
    irq_register(1, keyboard_irq);
    pic_clear_mask(1);
    printf("[kbd] PS/2 клавиатура готова (IRQ1)\n");
}

/* Неблокирующее чтение символа: 0 если буфер пуст. */
int keyboard_getchar(void) {
    if (kbd_tail == kbd_head) return 0;
    char c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return (int)(unsigned char)c;
}

int keyboard_has_input(void) {
    return kbd_tail != kbd_head;
}
