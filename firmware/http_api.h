#ifndef HTTP_API_H
#define HTTP_API_H

#include <stdint.h>

/* Poll for incoming HTTP requests on the CC3000 listening socket and serve them. */
void http_api_poll(uint8_t *fb_curr, uint8_t *fb_prev, uint8_t *fb_has_prev,
                   void (*beep_fn)(uint16_t, uint16_t));

#endif
