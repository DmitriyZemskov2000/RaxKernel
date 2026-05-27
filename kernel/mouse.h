#ifndef WEBOS_MOUSE_H
#define WEBOS_MOUSE_H

#ifdef __cplusplus
extern "C" {
#endif

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

void mouse_init(int screen_w, int screen_h);
void mouse_get(int* x, int* y, unsigned* buttons);

#ifdef __cplusplus
}
#endif

#endif
