/*
 * Minimal built-in 264x176 E-Ink status screens (boot splash + Wi-Fi OK with IP)
 */
#include "epd.h"
#include "ui.h"

static void set_px(uint8_t *fb, int x, int y, int black) {
    if (x < 0 || x >= (int)EPD_WIDTH || y < 0 || y >= (int)EPD_HEIGHT) return;
    int idx = y * ((int)EPD_WIDTH / 8) + (x >> 3);
    uint8_t mask = (uint8_t)(0x80u >> (x & 7));
    if (black) fb[idx] |= mask;
    else       fb[idx] &= (uint8_t)~mask;
}

static void fill_rect(uint8_t *fb, int x0, int y0, int w, int h, int black) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            set_px(fb, x, y, black);
}

/* Compact 5x7 uppercase + digits + punctuation glyph lookup */
static void get_glyph(char c, uint8_t g[7]) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    static const struct { char ch; uint8_t r[7]; } tbl[] = {
        {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
        {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
        {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
        {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
        {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
        {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
        {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
        {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
        {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
        {'J',{0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},
        {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
        {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
        {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
        {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
        {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
        {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
        {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
        {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
        {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
        {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
        {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
        {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
        {'W',{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
        {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
        {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
        {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
        {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
        {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
        {'2',{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}},
        {'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
        {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
        {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
        {'6',{0x0E,0x10,0x1E,0x11,0x11,0x11,0x0E}},
        {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
        {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
        {'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}},
        {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
        {':',{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
        {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
        {'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
    };
    for (int i = 0; i < 7; i++) g[i] = 0;
    for (uint32_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (tbl[i].ch == c) {
            for (int r = 0; r < 7; r++) g[r] = tbl[i].r[r];
            return;
        }
    }
}

static void draw_text(uint8_t *fb, int x, int y, const char *s, int scale, int black) {
    while (*s) {
        uint8_t g[7];
        get_glyph(*s++, g);
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (g[r] & (0x10u >> c)) {
                    fill_rect(fb, x + c * scale, y + r * scale, scale, scale, black);
                }
            }
        }
        x += 6 * scale;
    }
}

/* Draw a crisp 32x24 Wi-Fi signal icon centered at (cx, cy) */
static void draw_wifi_icon(uint8_t *fb, int cx, int cy) {
    /* Concentric upper arcs (y <= cy) + center dot */
    for (int dy = -22; dy <= 4; dy++) {
        for (int dx = -22; dx <= 22; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= 9) {
                set_px(fb, cx + dx, cy + dy, 1);
            } else if (dy < 0 && (dx < 0 ? -dx : dx) <= (-dy) + 3) {
                if ((d2 >= 64 && d2 <= 105) ||
                    (d2 >= 185 && d2 <= 245) ||
                    (d2 >= 360 && d2 <= 445)) {
                    set_px(fb, cx + dx, cy + dy, 1);
                }
            }
        }
    }
}

static void draw_frame_header(uint8_t *fb) {
    for (uint32_t i = 0; i < EPD_FB_SIZE; i++) fb[i] = 0;
    /* Outer border */
    fill_rect(fb, 4, 4, (int)EPD_WIDTH - 8, 3, 1);
    fill_rect(fb, 4, (int)EPD_HEIGHT - 7, (int)EPD_WIDTH - 8, 3, 1);
    fill_rect(fb, 4, 4, 3, (int)EPD_HEIGHT - 8, 1);
    fill_rect(fb, (int)EPD_WIDTH - 7, 4, 3, (int)EPD_HEIGHT - 8, 1);
    /* Top black banner */
    fill_rect(fb, 12, 12, (int)EPD_WIDTH - 24, 34, 1);
    draw_text(fb, 34, 22, "DISPLIO REBIRTH", 2, 0);
}

static void flush_ui(uint8_t *fb_curr, uint8_t *fb_prev, uint8_t *fb_has_prev, uint8_t full) {
    if (epd_refresh(*fb_has_prev ? fb_prev : 0, fb_curr, full) == 0) {
        for (uint32_t i = 0; i < EPD_FB_SIZE; i++) fb_prev[i] = fb_curr[i];
        *fb_has_prev = 1;
    }
}

void ui_draw_boot(uint8_t *fb_curr, uint8_t *fb_prev, uint8_t *fb_has_prev,
                  const char *ssid) {
    draw_frame_header(fb_curr);
    /* Decorative checkerboard test strip (mire) */
    for (int y = 56; y < 80; y++) {
        for (int x = 16; x < (int)EPD_WIDTH - 16; x++) {
            if (((x >> 3) ^ (y >> 3)) & 1) set_px(fb_curr, x, y, 1);
        }
    }
    if (ssid && ssid[0]) {
        draw_text(fb_curr, 24, 96, "CONNECTING WI-FI...", 2, 1);
        draw_text(fb_curr, 24, 126, ssid, 2, 1);
    } else {
        draw_text(fb_curr, 24, 96, "NO WI-FI CONFIG", 2, 1);
        draw_text(fb_curr, 24, 126, "RUN WIFI-SETUP", 2, 1);
    }
    draw_text(fb_curr, 24, 152, "FW V1.0 - USB READY", 1, 1);
    flush_ui(fb_curr, fb_prev, fb_has_prev, 1u);
}

static char *fmt_u8(char *p, uint8_t v) {
    if (v >= 100) *p++ = (char)('0' + v / 100);
    if (v >= 10)  *p++ = (char)('0' + (v / 10) % 10);
    *p++ = (char)('0' + v % 10);
    return p;
}

void ui_draw_wifi_ok(uint8_t *fb_curr, uint8_t *fb_prev, uint8_t *fb_has_prev,
                     const char *ssid, const uint8_t ip[4]) {
    draw_frame_header(fb_curr);
    draw_wifi_icon(fb_curr, 46, 90);
    draw_text(fb_curr, 84, 68, "WI-FI OK", 2, 1);
    if (ssid && ssid[0]) {
        draw_text(fb_curr, 84, 92, ssid, 2, 1);
    }
    char ip_str[24];
    char *p = ip_str;
    for (int i = 0; i < 4; i++) {
        if (i) *p++ = '.';
        p = fmt_u8(p, ip[i]);
    }
    *p = '\0';
    draw_text(fb_curr, 24, 126, ip_str, 2, 1);
    draw_text(fb_curr, 24, 152, "HTTP API READY - PORT 80", 1, 1);
    flush_ui(fb_curr, fb_prev, fb_has_prev, 0u);
}
