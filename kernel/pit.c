/*
 * pit.c — Programmable Interval Timer 8254.
 *
 * Базовая частота PIT — 1.193182 MHz. Делим её на divisor → получаем
 * частоту IRQ0. Поддерживаемый диапазон: 18 Гц .. 1.19 МГц.
 *
 * Для планировщика берём 100 Гц (10 мс quantum) — баланс между
 * отзывчивостью и накладными расходами на context switch.
 */

#include "types.h"
#include "io.h"
#include "pit.h"

#define PIT_FREQ      1193182u
#define PIT_CH0_DATA  0x40
#define PIT_CMD       0x43

/* Глобальный счётчик тиков — растёт в IRQ0-обработчике. */
static volatile u64 pit_ticks_count = 0;
static u32 pit_hz = 100;

void pit_init(u32 hz) {
    if (hz == 0) hz = 100;
    pit_hz = hz;

    u32 divisor = PIT_FREQ / hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;   /* минимум 18.2 Hz */
    if (divisor < 1)      divisor = 1;

    /* Command: channel 0, lo/hi access, mode 3 (square wave), binary */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0_DATA, (u8)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (u8)((divisor >> 8) & 0xFF));
}

void pit_tick_inc(void) { pit_ticks_count++; }
u64  pit_ticks(void)    { return pit_ticks_count; }
u32  pit_frequency(void){ return pit_hz; }

/* Грубая задержка через busy-wait по таймеру. Не для production,
   но для bring-up'а полезно. */
void pit_sleep_ms(u32 ms) {
    u64 target_ticks = pit_ticks_count + (u64)ms * pit_hz / 1000;
    if (target_ticks == pit_ticks_count) target_ticks++;
    while (pit_ticks_count < target_ticks) {
        /* sti+hlt атомарно: CPU обещает обработать одно прерывание
           между sti и hlt не успеет проснуться (т.н. interruptable hlt).
           После просыпания снова cli — мы могли быть вызваны из syscall
           context'а, где прерывания изначально были выключены. */
        __asm__ volatile("sti; hlt; cli");
    }
}
