#ifndef WEBOS_KEYBOARD_H
#define WEBOS_KEYBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

void keyboard_init(void);
int  keyboard_getchar(void);    /* 0 если нет ввода */
int  keyboard_has_input(void);

#ifdef __cplusplus
}
#endif

#endif
