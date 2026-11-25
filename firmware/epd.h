#ifndef EPD_H
#define EPD_H

#include <stdint.h>

#define EPD_WIDTH   264u
#define EPD_HEIGHT  176u
#define EPD_FB_SIZE 5808u /* 264 * 176 / 8 */

void epd_init_pins(void);
int  epd_refresh(const uint8_t *old_fb, const uint8_t *new_fb, uint8_t full_clear);

#endif
