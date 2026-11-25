/*
 * Minimal HTTP/1.1 REST API served over TI CC3000 TCP socket (default port 80)
 * Designed as the clean contract for Home Assistant (displio-rebirth-ha):
 *   GET  /status       -> JSON with Si7021 temp/RH, MMA8452Q accel/orientation, VBAT/VDDA, button, vbus, ip, mac
 *   POST /display      -> 5808-byte raw 1-bpp 264x176 framebuffer (?full=1 for full clear)
 *   POST /beep         -> buzzes piezo (?freq=2400&ms=80)
 *   GET  /             -> short text/plain summary
 */
#include "stm32f103.h"
#include "cc3000.h"
#include "epd.h"
#include "sensors.h"
#include "bus_scan.h"
#include "http_api.h"

static int starts_with(const char *s, const char *p) {
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}

static char *append_str(char *dst, const char *s) {
    while (*s) *dst++ = *s++;
    return dst;
}

static char *append_int(char *dst, int32_t v) {
    char tmp[12];
    int i = 0;
    if (v < 0) { *dst++ = '-'; v = -v; }
    if (v == 0) { *dst++ = '0'; return dst; }
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) *dst++ = tmp[--i];
    return dst;
}

static char *append_hex2(char *dst, uint8_t v) {
    static const char h[16] = "0123456789abcdef";
    *dst++ = h[v >> 4];
    *dst++ = h[v & 0xFu];
    return dst;
}

static uint16_t parse_query_u16(const char *req, const char *key, uint16_t def) {
    for (const char *p = req; *p && *p != '\r' && *p != '\n'; p++) {
        if (starts_with(p, key)) {
            p += 0;
            while (*key) { p++; key++; }
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10u + (uint32_t)(*p++ - '0'); }
            return (uint16_t)v;
        }
    }
    return def;
}

static void read_si7021(int32_t *temp_c_x100, int32_t *rh_x10) {
    *temp_c_x100 = 0;
    *rh_x10 = 0;
    uint8_t cmd_t = 0xF3u, cmd_h = 0xF5u, raw[3];
    if (i2c1_xfer(0x40u, &cmd_t, 1, raw, 3, 25u) == 0) {
        uint32_t rt = ((uint32_t)raw[0] << 8) | (raw[1] & 0xFCu);
        *temp_c_x100 = (int32_t)((17572u * rt) / 65536u) - 4685;
    }
    if (i2c1_xfer(0x40u, &cmd_h, 1, raw, 3, 25u) == 0) {
        uint32_t rh = ((uint32_t)raw[0] << 8) | (raw[1] & 0xFCu);
        int32_t v = (int32_t)((1250u * rh) / 65536u) - 60;
        if (v < 0) v = 0;
        if (v > 1000) v = 1000;
        *rh_x10 = v;
    }
}

static void send_http(int32_t sd, const char *status, const char *ctype, const char *body) {
    char buf[480];
    uint16_t blen = 0;
    while (body[blen]) blen++;
    char *p = buf;
    p = append_str(p, "HTTP/1.1 ");
    p = append_str(p, status);
    p = append_str(p, "\r\nContent-Type: ");
    p = append_str(p, ctype);
    p = append_str(p, "\r\nContent-Length: ");
    p = append_int(p, blen);
    p = append_str(p, "\r\nConnection: close\r\n\r\n");
    uint16_t hlen = (uint16_t)(p - buf);
    if ((uint16_t)(hlen + blen) <= 470u) {
        for (uint16_t i = 0; i < blen; i++) p[i] = body[i];
        cc3000_tcp_send(sd, (const uint8_t *)buf, (uint16_t)(hlen + blen));
    } else {
        cc3000_tcp_send(sd, (const uint8_t *)buf, hlen);
        if (blen > 0) cc3000_tcp_send(sd, (const uint8_t *)body, blen);
    }
}

static void handle_status(int32_t sd) {
    int32_t temp_x100 = 0, rh_x10 = 0;
    read_si7021(&temp_x100, &rh_x10);

    int16_t ax = 0, ay = 0, az = 0;
    uint8_t pl = 0;
    mma8452_read(&ax, &ay, &az, &pl);

    /* Battery via PC13 -> PA0 */
    gpio_config(GPIOC, 13, GPIO_MODE_OUT_PP_50);
    gpio_set(GPIOC, 13);
    delay_ms(10);
    uint16_t idr[3], adc[11];
    scan_gpio_adc(idr, adc);
    gpio_clear(GPIOC, 13);
    uint32_t vdda_mv = adc[10] ? (1200u * 4095u / adc[10]) : 3000u;
    uint32_t pa0_mv  = (adc[0] * vdda_mv) / 4095u;

    char json[320];
    char *p = json;
    p = append_str(p, "{\"device\":\"displio-rebirth\",\"fw\":\"1.0\",\"uptime_ms\":");
    p = append_int(p, (int32_t)g_ms);
    p = append_str(p, ",\"temp_c\":");
    p = append_int(p, temp_x100 / 100);
    *p++ = '.';
    int32_t tfrac = temp_x100 % 100; if (tfrac < 0) tfrac = -tfrac;
    if (tfrac < 10) *p++ = '0';
    p = append_int(p, tfrac);
    p = append_str(p, ",\"humidity\":");
    p = append_int(p, rh_x10 / 10);
    *p++ = '.';
    p = append_int(p, rh_x10 % 10);
    p = append_str(p, ",\"vdda_mv\":");
    p = append_int(p, (int32_t)vdda_mv);
    p = append_str(p, ",\"vbat_adc_mv\":");
    p = append_int(p, (int32_t)pa0_mv);
    p = append_str(p, ",\"accel_mg\":[");
    p = append_int(p, ax); *p++ = ',';
    p = append_int(p, ay); *p++ = ',';
    p = append_int(p, az);
    p = append_str(p, "],\"orientation\":");
    p = append_int(p, (pl >> 1) & 3);
    p = append_str(p, ",\"button\":");
    p = append_int(p, gpio_read(GPIOA, 2));
    p = append_str(p, ",\"vbus\":");
    p = append_int(p, gpio_read(GPIOA, 9));
    p = append_str(p, ",\"ip\":\"");
    for (int i = 0; i < 4; i++) {
        if (i) *p++ = '.';
        p = append_int(p, g_cc.ip[i]);
    }
    p = append_str(p, "\",\"mac\":\"");
    for (int i = 0; i < 6; i++) {
        if (i) *p++ = ':';
        p = append_hex2(p, g_cc.mac[i]);
    }
    p = append_str(p, "\"}\n");
    *p = '\0';
    send_http(sd, "200 OK", "application/json", json);
}

void http_api_poll(uint8_t *fb_curr, uint8_t *fb_prev, uint8_t *fb_has_prev,
                   void (*beep_fn)(uint16_t, uint16_t)) {
    if (g_cc.state != CC_STATE_DHCP_OK || g_cc.listen_sd < 0) return;

    int32_t csd = cc3000_tcp_accept_nb(g_cc.listen_sd);
    if (csd < 0) return;

    uint8_t req[256];
    int32_t n = cc3000_tcp_recv(csd, req, sizeof(req) - 1u, 1500u);
    if (n <= 0) { cc3000_tcp_close(csd); return; }
    req[n] = '\0';

    if (starts_with((const char *)req, "GET /status")) {
        handle_status(csd);
    } else if (starts_with((const char *)req, "POST /beep")) {
        uint16_t freq = parse_query_u16((const char *)req, "freq=", 2400u);
        uint16_t dur  = parse_query_u16((const char *)req, "ms=", 80u);
        if (beep_fn) beep_fn(freq, dur);
        send_http(csd, "200 OK", "application/json", "{\"ok\":true}\n");
    } else if (starts_with((const char *)req, "POST /display")) {
        uint8_t full = (uint8_t)parse_query_u16((const char *)req, "full=", 0u);
        /* Find end of HTTP headers (\r\n\r\n) */
        int32_t body_off = -1;
        for (int32_t i = 0; i + 3 < n; i++) {
            if (req[i] == '\r' && req[i+1] == '\n' && req[i+2] == '\r' && req[i+3] == '\n') {
                body_off = i + 4;
                break;
            }
        }
        uint32_t got = 0;
        if (body_off >= 0) {
            for (int32_t i = body_off; i < n && got < EPD_FB_SIZE; i++)
                fb_curr[got++] = req[i];
        }
        /* Read remaining framebuffer bytes */
        uint32_t t0 = g_ms;
        while (got < EPD_FB_SIZE && (uint32_t)(g_ms - t0) < 8000u) {
            uint16_t want = (uint16_t)((EPD_FB_SIZE - got > 200u) ? 200u : (EPD_FB_SIZE - got));
            int32_t r = cc3000_tcp_recv(csd, fb_curr + got, want, 1500u);
            if (r <= 0) break;
            got += (uint32_t)r;
        }
        if (got == EPD_FB_SIZE) {
            send_http(csd, "200 OK", "application/json", "{\"ok\":true}\n");
            cc3000_tcp_close(csd);
            if (epd_refresh(*fb_has_prev ? fb_prev : 0, fb_curr, full) == 0) {
                for (uint32_t i = 0; i < EPD_FB_SIZE; i++) fb_prev[i] = fb_curr[i];
                *fb_has_prev = 1;
            }
            return;
        }
        send_http(csd, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"need 5808 bytes\"}\n");
    } else if (starts_with((const char *)req, "GET / ")) {
        send_http(csd, "200 OK", "text/plain", "displio-rebirth ok — GET /status | POST /display | POST /beep\n");
    } else {
        send_http(csd, "404 Not Found", "application/json", "{\"error\":\"not_found\"}\n");
    }
    cc3000_tcp_close(csd);
}
