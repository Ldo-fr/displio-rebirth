/*
 * displio-rebirth custom firmware (Phase 3)
 *
 * USB HID command protocol (64-byte reports, CRC-16/EN-13757 in bytes 62..63):
 *   Request:  [0xA5, cmd, payload (60B), crc16_le (2B)]
 *   Response: [0x5A, cmd, status (0=OK), payload (59B), crc16_le (2B)]
 *
 * Commands:
 *   0x10 PING/INFO      -> "REBIRTH" + version + uptime + button + vbus
 *   0x11 BEEP           -> [freq_hz:u16, dur_ms:u16] on PB0 (TIM3_CH3)
 *   0x12 FB_WRITE       -> [offset:u16, len:u8 (<=56), data...]
 *   0x13 FB_CLEAR       -> [pattern:u8]
 *   0x14 EPD_REFRESH    -> [full_clear:u8]
 *   0x15 SCAN_I2C       -> [remap:u8 (0=PB6/7, 1=PB8/9)] -> [count, (addr,0F,0D,00)...]
 *   0x16 SCAN_GPIO_ADC  -> [GPIOA/B/C IDR (3xu16), ADC ch0..9+Vref (11xu16)]
 *   0x17 SCAN_SPI2      -> [count, (port_pin, id0, id1, id2)...]
 *   0x7F BOOTLOADER     -> sets BKP_DR1=0xB007 and resets into USB bootloader!
 */
#include "stm32f103.h"
#include "usb_hid.h"
#include "epd.h"
#include "bus_scan.h"
#include "sensors.h"
#include "cc3000.h"
#include "wifi_cfg.h"
#include "http_api.h"
#include "ui.h"

#define MAGIC_REQ  0xA5u
#define MAGIC_RESP 0x5Au

#define CMD_INFO       0x10u
#define CMD_BEEP       0x11u
#define CMD_FB_WRITE   0x12u
#define CMD_FB_CLEAR   0x13u
#define CMD_EPD_SHOW   0x14u
#define CMD_SCAN_I2C   0x15u
#define CMD_SCAN_IO    0x16u
#define CMD_SCAN_SPI2  0x17u
#define CMD_ACCEL      0x20u
#define CMD_I2C_XFER   0x21u
#define CMD_GPIO_CTRL  0x22u
#define CMD_SPI2_XFER  0x23u
#define CMD_WIFI_SSID  0x30u
#define CMD_WIFI_PASS  0x31u
#define CMD_WIFI_STAT  0x32u
#define CMD_WIFI_CONN  0x33u
#define CMD_WIFI_OFF   0x34u
#define CMD_BOOTLOADER 0x7Fu

static wifi_cfg_t g_wcfg;
static uint8_t    g_wcfg_loaded = 0;
static uint32_t   g_conn_start_ms = 0;
static uint8_t    g_wifi_ui_shown = 0;

static uint8_t g_fb_curr[EPD_FB_SIZE];
static uint8_t g_fb_prev[EPD_FB_SIZE];
static uint8_t g_fb_has_prev = 0;

static uint16_t crc16_en13757(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x3D65u) : (uint16_t)(crc << 1);
    }
    return (uint16_t)(crc ^ 0xFFFFu);
}

static void buzzer_beep(uint16_t freq_hz, uint16_t dur_ms) {
    if (freq_hz < 100u || freq_hz > 10000u || dur_ms == 0u) return;
    if (dur_ms > 2000u) dur_ms = 2000u;

    RCC_APB1ENR |= (1u << 1); /* TIM3EN */
    gpio_config(GPIOB, 0, GPIO_MODE_AF_PP_50); /* PB0 = TIM3_CH3 */

    uint32_t arr = (7200000u / freq_hz) - 1u; /* PSC=9 -> 7.2 MHz */
    TIM3->CR1   = 0;
    TIM3->PSC   = 9u;
    TIM3->ARR   = arr;
    TIM3->CCR3  = arr >> 1;
    TIM3->CCMR2 = (6u << 4) | (1u << 3); /* OC3M = PWM1, OC3PE */
    TIM3->CCER  = (1u << 8);             /* CC3E */
    TIM3->EGR   = 1u;                    /* UG */
    TIM3->CR1   = 1u;                    /* CEN */

    delay_ms(dur_ms);

    TIM3->CR1  = 0;
    TIM3->CCER = 0;
    gpio_config(GPIOB, 0, GPIO_MODE_OUT_PP_50);
    gpio_clear(GPIOB, 0);
}

static void send_reply(uint8_t cmd, uint8_t status, const uint8_t *payload, uint8_t len) {
    uint8_t pkt[64];
    for (int i = 0; i < 64; i++) pkt[i] = 0;
    pkt[0] = MAGIC_RESP;
    pkt[1] = cmd;
    pkt[2] = status;
    if (len > 59u) len = 59u;
    for (uint8_t i = 0; i < len; i++) pkt[3 + i] = payload[i];
    uint16_t crc = crc16_en13757(pkt, 62);
    pkt[62] = (uint8_t)(crc & 0xFFu);
    pkt[63] = (uint8_t)(crc >> 8);
    usb_hid_send(pkt);
}

static void handle_packet(const uint8_t rx[64]) {
    uint16_t rx_crc = (uint16_t)(rx[62] | (rx[63] << 8));
    if (rx[0] != MAGIC_REQ || crc16_en13757(rx, 62) != rx_crc) {
        send_reply(rx[1], 0xEEu, 0, 0);
        return;
    }

    uint8_t cmd = rx[1];
    uint8_t out[59];
    for (int i = 0; i < 59; i++) out[i] = 0;

    switch (cmd) {
    case CMD_INFO: {
        static const char tag[8] = "REBIRTH1";
        for (int i = 0; i < 8; i++) out[i] = (uint8_t)tag[i];
        uint32_t up = g_ms;
        out[8]  = (uint8_t)(up & 0xFFu);
        out[9]  = (uint8_t)((up >> 8) & 0xFFu);
        out[10] = (uint8_t)((up >> 16) & 0xFFu);
        out[11] = (uint8_t)((up >> 24) & 0xFFu);
        out[12] = gpio_read(GPIOA, 2); /* button */
        out[13] = gpio_read(GPIOA, 9); /* VBUS */
        out[14] = g_fb_has_prev;
        send_reply(cmd, 0, out, 16);
        break;
    }
    case CMD_BEEP: {
        uint16_t freq = (uint16_t)(rx[2] | (rx[3] << 8));
        uint16_t dur  = (uint16_t)(rx[4] | (rx[5] << 8));
        buzzer_beep(freq, dur);
        send_reply(cmd, 0, 0, 0);
        break;
    }
    case CMD_FB_WRITE: {
        uint16_t off = (uint16_t)(rx[2] | (rx[3] << 8));
        uint8_t  len = rx[4];
        if (len > 56u || (uint32_t)off + len > EPD_FB_SIZE) {
            send_reply(cmd, 0x02u, 0, 0);
            break;
        }
        for (uint8_t i = 0; i < len; i++) g_fb_curr[off + i] = rx[5 + i];
        send_reply(cmd, 0, 0, 0);
        break;
    }
    case CMD_FB_CLEAR: {
        uint8_t pat = rx[2];
        for (uint32_t i = 0; i < EPD_FB_SIZE; i++) g_fb_curr[i] = pat;
        send_reply(cmd, 0, 0, 0);
        break;
    }
    case CMD_EPD_SHOW: {
        uint8_t full = rx[2];
        int rc = epd_refresh(g_fb_has_prev ? g_fb_prev : 0, g_fb_curr, full);
        if (rc == 0) {
            for (uint32_t i = 0; i < EPD_FB_SIZE; i++) g_fb_prev[i] = g_fb_curr[i];
            g_fb_has_prev = 1;
        }
        out[0] = (uint8_t)(rc & 0xFF);
        send_reply(cmd, rc == 0 ? 0u : 0x03u, out, 1);
        break;
    }
    case CMD_SCAN_I2C: {
        uint8_t remap = rx[2] ? 1u : 0u;
        uint8_t n = scan_i2c1(remap, out + 1, 14);
        out[0] = n;
        send_reply(cmd, 0, out, (uint8_t)(1u + n * 4u));
        break;
    }
    case CMD_SCAN_IO: {
        uint16_t idr[3], adc[11];
        scan_gpio_adc(idr, adc);
        for (int i = 0; i < 3; i++) {
            out[i * 2]     = (uint8_t)(idr[i] & 0xFFu);
            out[i * 2 + 1] = (uint8_t)(idr[i] >> 8);
        }
        for (int i = 0; i < 11; i++) {
            out[6 + i * 2]     = (uint8_t)(adc[i] & 0xFFu);
            out[6 + i * 2 + 1] = (uint8_t)(adc[i] >> 8);
        }
        send_reply(cmd, 0, out, 28);
        break;
    }
    case CMD_SCAN_SPI2: {
        uint8_t n = scan_spi2_jedec(out + 1, 14);
        out[0] = n;
        send_reply(cmd, 0, out, (uint8_t)(1u + n * 4u));
        break;
    }
    case CMD_ACCEL: {
        int16_t x = 0, y = 0, z = 0;
        uint8_t pl = 0;
        int rc = mma8452_read(&x, &y, &z, &pl);
        out[0] = (uint8_t)(x & 0xFF); out[1] = (uint8_t)((x >> 8) & 0xFF);
        out[2] = (uint8_t)(y & 0xFF); out[3] = (uint8_t)((y >> 8) & 0xFF);
        out[4] = (uint8_t)(z & 0xFF); out[5] = (uint8_t)((z >> 8) & 0xFF);
        out[6] = pl;
        send_reply(cmd, rc == 0 ? 0u : 0x04u, out, 7);
        break;
    }
    case CMD_I2C_XFER: {
        /* rx: [addr, tx_len, rx_len, wait_ms, tx_bytes...] */
        uint8_t addr    = rx[2];
        uint8_t tx_len  = rx[3];
        uint8_t rx_len  = rx[4];
        uint8_t wait_ms = rx[5];
        if (tx_len > 54u || rx_len > 58u) { send_reply(cmd, 0x02u, 0, 0); break; }
        int rc = i2c1_xfer(addr, rx + 6, tx_len, out, rx_len, wait_ms);
        send_reply(cmd, rc == 0 ? 0u : (uint8_t)(-rc), out, rx_len);
        break;
    }
    case CMD_GPIO_CTRL: {
        /* rx: [port(0=A,1=B,2=C), pin(0..15), mode(0..15), val(0/1), wait_ms]
         * returns GPIOA/B/C IDR before & after (6 x u16 = 12B) */
        GPIO_TypeDef *ports[3] = { GPIOA, GPIOB, GPIOC };
        uint8_t port = rx[2], pin = rx[3], mode = rx[4], val = rx[5], wms = rx[6];
        if (port >= 3u || pin >= 16u) { send_reply(cmd, 0x02u, 0, 0); break; }
        uint16_t bA = (uint16_t)GPIOA->IDR, bB = (uint16_t)GPIOB->IDR, bC = (uint16_t)GPIOC->IDR;
        gpio_config(ports[port], pin, mode & 0x0Fu);
        if (val) gpio_set(ports[port], pin); else gpio_clear(ports[port], pin);
        if (wms) delay_ms(wms);
        uint16_t aA = (uint16_t)GPIOA->IDR, aB = (uint16_t)GPIOB->IDR, aC = (uint16_t)GPIOC->IDR;
        uint16_t vals[6] = { bA, bB, bC, aA, aB, aC };
        for (int i = 0; i < 6; i++) {
            out[i * 2]     = (uint8_t)(vals[i] & 0xFFu);
            out[i * 2 + 1] = (uint8_t)(vals[i] >> 8);
        }
        send_reply(cmd, 0, out, 12);
        break;
    }
    case CMD_SPI2_XFER: {
        /* rx: [cs_port_pin, spi_mode, br_div, len(<=54), tx_bytes...] */
        uint8_t cs = rx[2], mode = rx[3], br = rx[4], len = rx[5];
        if (len > 54u) { send_reply(cmd, 0x02u, 0, 0); break; }
        spi2_init(mode, br);
        spi2_xfer_buf(cs, rx + 6, out, len);
        send_reply(cmd, 0, out, len);
        break;
    }
    case CMD_WIFI_SSID: {
        /* rx: [sec(0..3), port_lo, port_hi, ssid_len(<=32), ssid...] */
        g_wcfg.sec  = rx[2];
        g_wcfg.port = (uint16_t)(rx[3] | (rx[4] << 8));
        if (g_wcfg.port == 0u) g_wcfg.port = 80u;
        uint8_t n = rx[5]; if (n > 32u) n = 32u;
        for (uint8_t i = 0; i < 33u; i++) g_wcfg.ssid[i] = (i < n) ? (char)rx[6u + i] : '\0';
        send_reply(cmd, 0, 0, 0);
        break;
    }
    case CMD_WIFI_PASS: {
        /* rx: [part(0=bytes 0..55, 1=bytes 56..63), len, data..., save_flag] */
        uint8_t part = rx[2], len = rx[3], save = rx[4];
        uint8_t off = (part == 0u) ? 0u : 56u;
        if (part == 0u) { for (int i = 0; i < 65; i++) g_wcfg.pass[i] = '\0'; }
        for (uint8_t i = 0; i < len && (off + i) < 64u; i++) g_wcfg.pass[off + i] = (char)rx[5u + i];
        if (save) {
            wifi_cfg_save(&g_wcfg);
            g_wcfg_loaded = 1;
        }
        send_reply(cmd, 0, 0, 0);
        break;
    }
    case CMD_WIFI_STAT: {
        cc3000_poll();
        out[0] = (uint8_t)g_cc.state;
        out[1] = g_wcfg_loaded ? g_wcfg.sec : 0xFFu;
        out[2] = g_cc.ip[0]; out[3] = g_cc.ip[1]; out[4] = g_cc.ip[2]; out[5] = g_cc.ip[3];
        for (int i = 0; i < 6; i++) out[6 + i] = g_cc.mac[i];
        out[12] = g_cc.sp_ver[0]; out[13] = g_cc.sp_ver[1];
        out[14] = (uint8_t)(g_wcfg.port & 0xFFu);
        out[15] = (uint8_t)(g_wcfg.port >> 8);
        out[16] = (g_cc.state == CC_STATE_DHCP_OK && g_cc.listen_sd >= 0) ? 1u : 0u;
        for (int i = 0; i < 32; i++) out[17 + i] = (uint8_t)g_wcfg.ssid[i];
        out[49] = g_cc.gw[0]; out[50] = g_cc.gw[1]; out[51] = g_cc.gw[2]; out[52] = g_cc.gw[3];
        out[53] = (uint8_t)g_cc.last_wlan_st;
        out[54] = g_cc.ping_ok;
        out[55] = g_cc.ping_fail_cnt;
        out[56] = g_cc.reconn_cnt;
        send_reply(cmd, 0, out, 57);
        break;
    }
    case CMD_WIFI_CONN: {
        g_wifi_ui_shown = 0;
        int rc = cc3000_init();
        if (rc == 0 && (g_wcfg_loaded || wifi_cfg_load(&g_wcfg) == 0)) {
            g_wcfg_loaded = 1;
            g_conn_start_ms = g_ms;
            cc3000_connect(g_wcfg.ssid, g_wcfg.pass, g_wcfg.sec);
        }
        out[0] = (uint8_t)(rc & 0xFF);
        send_reply(cmd, rc == 0 ? 0u : 0x06u, out, 1);
        break;
    }
    case CMD_WIFI_OFF: {
        cc3000_power_off();
        send_reply(cmd, 0, 0, 0);
        break;
    }
    case CMD_BOOTLOADER: {
        send_reply(cmd, 0, 0, 0);
        delay_ms(50);
        enter_bootloader();
        break;
    }
    default:
        send_reply(cmd, 0xFFu, 0, 0);
        break;
    }
}

int main(void) {
    /* Configure button PA2 & VBUS PA9 as inputs */
    gpio_config(GPIOA, 2, GPIO_MODE_IN_FLOAT);
    gpio_config(GPIOA, 9, GPIO_MODE_IN_FLOAT);

    epd_init_pins();
    mma8452_init();
    usb_hid_init();

    /* Short friendly startup chirp (2200 Hz, 40 ms) so user knows custom fw booted! */
    buzzer_beep(2200, 40);

    /* Auto-connect Wi-Fi if credentials are saved in Flash */
    if (wifi_cfg_load(&g_wcfg) == 0) {
        g_wcfg_loaded = 1;
    }

    /* Show boot splash + test strip (mire) on E-Ink */
    ui_draw_boot(g_fb_curr, g_fb_prev, &g_fb_has_prev, g_wcfg_loaded ? g_wcfg.ssid : 0);

    if (g_wcfg_loaded && cc3000_init() == 0) {
        g_conn_start_ms = g_ms;
        cc3000_connect(g_wcfg.ssid, g_wcfg.pass, g_wcfg.sec);
    }

    uint8_t rx[64];
    uint32_t last_http_ms = 0;
    uint32_t last_reconn_ms = 0;
    uint32_t last_wd_ms = 0;
    uint32_t ping_pause_until = 0;
    uint8_t  ping_pending = 0;
    g_conn_start_ms = g_ms;
    for (;;) {
        if (usb_hid_rx_ready()) {
            usb_hid_read(rx);
            handle_packet(rx);
        }
        cc3000_poll();
        if (g_cc.state == CC_STATE_DHCP_OK && g_cc.listen_sd < 0) {
            cc3000_get_ipconfig();
            cc3000_tcp_listen(g_wcfg.port ? g_wcfg.port : 80u);
            last_wd_ms = g_ms;
            g_cc.ping_fail_cnt = 0;
            ping_pending = 0;
            if (!g_wifi_ui_shown) {
                g_wifi_ui_shown = 1;
                ui_draw_wifi_ok(g_fb_curr, g_fb_prev, &g_fb_has_prev, g_wcfg.ssid, g_cc.ip);
            }
        }
        /* Poll HTTP accept at 4 Hz (250 ms) — plenty fast for REST, keeps CC3000 CPU free for ICMP/ARP */
        if (g_cc.state == CC_STATE_DHCP_OK &&
            (int32_t)(g_ms - ping_pause_until) >= 0 &&
            (uint32_t)(g_ms - last_http_ms) >= 250u) {
            last_http_ms = g_ms;
            http_api_poll(g_fb_curr, g_fb_prev, &g_fb_has_prev, buzzer_beep);
        }
        /* Wi-Fi health watchdog every 60s: check WLAN_STATUS_CONNECTED (3) + non-overlapping gateway ping */
        if (g_wcfg_loaded && g_cc.state == CC_STATE_DHCP_OK && (uint32_t)(g_ms - last_wd_ms) >= 60000u) {
            last_wd_ms = g_ms;
            if (ping_pending && !g_cc.ping_ok) {
                g_cc.ping_fail_cnt++;
            }
            int st = cc3000_wlan_status();
            g_cc.last_wlan_st = (int8_t)st;
            if (st != 3 || g_cc.ping_fail_cnt >= 3u) {
                g_cc.state = CC_STATE_ERROR;
                g_cc.listen_sd = -1;
            } else {
                cc3000_ping_gw();
                ping_pending = 1;
                /* Pause OP_ACCEPT for 1.5s while CC3000 processes the 1s gateway ping */
                ping_pause_until = g_ms + 1500u;
            }
        }
        /* Stuck connecting/DHCP timeout (25s) -> force hardware power-cycle */
        if (g_wcfg_loaded &&
            (g_cc.state == CC_STATE_CONNECTING || g_cc.state == CC_STATE_CONNECTED) &&
            (uint32_t)(g_ms - g_conn_start_ms) >= 25000u) {
            g_cc.state = CC_STATE_ERROR;
        }
        /* Auto-reconnect (with full hardware reset on CC_STATE_ERROR) */
        if (g_wcfg_loaded &&
            (g_cc.state == CC_STATE_INIT || g_cc.state == CC_STATE_ERROR) &&
            (uint32_t)(g_ms - last_reconn_ms) >= 5000u) {
            last_reconn_ms = g_ms;
            uint8_t prev_reconn = g_cc.reconn_cnt;
            if (g_cc.state == CC_STATE_ERROR) {
                if (cc3000_init() != 0) continue;
            }
            g_cc.reconn_cnt = (uint8_t)(prev_reconn + 1u);
            g_conn_start_ms = g_ms;
            ping_pending = 0;
            cc3000_connect(g_wcfg.ssid, g_wcfg.pass, g_wcfg.sec);
        }
        /* Safety: holding button PA2 for 2 seconds while running also enters bootloader */
        if (gpio_read(GPIOA, 2)) {
            uint32_t t0 = g_ms;
            while (gpio_read(GPIOA, 2)) {
                if ((uint32_t)(g_ms - t0) > 2000u) {
                    buzzer_beep(1500, 60);
                    enter_bootloader();
                }
            }
        }
    }
}
