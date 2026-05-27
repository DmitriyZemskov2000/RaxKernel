/*
 * kputs.c — нижний уровень вывода для libc.
 *
 * libc's printf/puts вызывает kputs_raw(buf, n) — это единственная
 * платформо-зависимая точка. В ядре пишем одновременно в VGA и в serial.
 * В userspace тот же символ будет другой — он сделает syscall write(1, ...).
 */

#include "types.h"
#include "vga.h"
#include "serial.h"

void kputs_raw(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        vga_putc(c);
        serial_putc(c);
        if (c == '\n') serial_putc('\r');   /* CRLF для serial-терминалов */
    }
}
