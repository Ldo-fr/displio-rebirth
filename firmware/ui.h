#ifndef UI_H
#define UI_H

#include <stdint.h>

void ui_draw_boot(uint8_t *fb_curr, uint8_t *fb_prev, uint8_t *fb_has_prev,
                  const char *ssid);
void ui_draw_wifi_ok(uint8_t *fb_curr, uint8_t *fb_prev, uint8_t *fb_has_prev,
                     const char *ssid, const uint8_t ip[4]);

#endif
