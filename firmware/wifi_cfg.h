#ifndef WIFI_CFG_H
#define WIFI_CFG_H

#include <stdint.h>

/* Stored in Flash page 126 (0x0801F800..0x0801FBFF), untouched by bootloader CRC page 127 */
#define WIFI_CFG_ADDR  0x0801F800u
#define WIFI_CFG_MAGIC 0x57494632u /* "WIF2" */

typedef struct {
    uint32_t magic;
    uint8_t  sec;
    uint8_t  reserved;
    uint16_t port;
    char     ssid[33];
    char     pass[65];
    uint8_t  ip[4];    /* 0.0.0.0 = DHCP, non-zero = static IP */
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint16_t pad;
} wifi_cfg_t;

int wifi_cfg_load(wifi_cfg_t *out);
int wifi_cfg_save(const wifi_cfg_t *cfg);

#endif
