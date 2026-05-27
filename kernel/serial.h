#ifndef WEBOS_SERIAL_H
#define WEBOS_SERIAL_H

#include "types.h"

void serial_init(void);
void serial_putc(char c);
void serial_write(const char* s);

#endif
