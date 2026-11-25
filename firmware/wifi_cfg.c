#include "stm32f103.h"
#include "wifi_cfg.h"

#define F1_FLASH_KEYR REG32(0x40022004u)
#define F1_FLASH_SR   REG32(0x4002200Cu)
#define F1_FLASH_CR   REG32(0x40022010u)
#define F1_FLASH_AR   REG32(0x40022014u)

int wifi_cfg_load(wifi_cfg_t *out) {
    const wifi_cfg_t *stored = (const wifi_cfg_t *)WIFI_CFG_ADDR;
    if (stored->magic != WIFI_CFG_MAGIC || stored->ssid[0] == '\0') return -1;
    *out = *stored;
    out->ssid[32] = '\0';
    out->pass[64] = '\0';
    if (out->port == 0u) out->port = 80u;
    return 0;
}

int wifi_cfg_save(const wifi_cfg_t *cfg) {
    wifi_cfg_t tmp = *cfg;
    tmp.magic = WIFI_CFG_MAGIC;
    tmp.ssid[32] = '\0';
    tmp.pass[64] = '\0';
    if (tmp.port == 0u) tmp.port = 80u;

    if (F1_FLASH_CR & (1u << 7)) {
        F1_FLASH_KEYR = 0x45670123u;
        F1_FLASH_KEYR = 0xCDEF89ABu;
    }
    F1_FLASH_SR = 0x34u;

    /* Erase 1 KiB page at 0x0801F800 */
    F1_FLASH_CR = (1u << 1); /* PER */
    F1_FLASH_AR = WIFI_CFG_ADDR;
    F1_FLASH_CR = (1u << 1) | (1u << 6); /* PER | STRT */
    while (F1_FLASH_SR & 1u) {}
    F1_FLASH_CR = 0;

    /* Program halfwords */
    const uint16_t *src = (const uint16_t *)&tmp;
    volatile uint16_t *dst = (volatile uint16_t *)WIFI_CFG_ADDR;
    uint32_t n = (sizeof(tmp) + 1u) / 2u;
    F1_FLASH_CR = 1u; /* PG */
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
        while (F1_FLASH_SR & 1u) {}
    }
    F1_FLASH_CR = 0;
    return 0;
}
