/*
 * setjmp.h — нелокальные переходы.
 *
 * Используется для exception-style escape в коде, где нет C++ исключений.
 * Реализация на ASM.
 */
#ifndef _SETJMP_H
#define _SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Структура хранит callee-saved регистры x86_64:
 * rbx, rbp, r12, r13, r14, r15 + RSP + RIP.
 * 8 64-битных значений = 64 байта. Плюс выравнивание.
 */
typedef long jmp_buf[8];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
