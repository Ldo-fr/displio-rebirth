/*
 * Pervasive Displays 2.7" G2 COG E-Ink driver (264x176, 1-bpp)
 * Pinout & register sequence verified directly from Displio V1.0 bootloader.
 *
 *   SPI1: PA5=SCK, PA6=MISO, PA7=MOSI
 *   PB11=CS (active LOW), PB10=BUSY (active HIGH)
 *   PB1=PANEL_ON (active LOW), PB2=RESET (active LOW)
 *   PA3=BORDER, PA4=DISCHARGE
 */
#include "stm32f103.h"
#include "epd.h"

#define EPD_CS_LOW()         gpio_clear(GPIOB, 11)
#define EPD_CS_HIGH()        gpio_set(GPIOB, 11)
#define EPD_BUSY()           gpio_read(GPIOB, 10)
#define EPD_PANEL_ON()       gpio_clear(GPIOB, 1)
#define EPD_PANEL_OFF()      gpio_set(GPIOB, 1)
#define EPD_RST_LOW()        gpio_clear(GPIOB, 2)
#define EPD_RST_HIGH()       gpio_set(GPIOB, 2)
#define EPD_BORDER_LOW()     gpio_clear(GPIOA, 3)
#define EPD_BORDER_HIGH()    gpio_set(GPIOA, 3)
#define EPD_DISCHARGE_LOW()  gpio_clear(GPIOA, 4)
#define EPD_DISCHARGE_HIGH() gpio_set(GPIOA, 4)

static uint8_t spi1_xfer(uint8_t b) {
    SPI1->DR = b;
    while (SPI1->SR & (1u << 7)) {} /* wait BSY=0 */
    return (uint8_t)SPI1->DR;
}

static void epd_wait_busy(void) {
    uint32_t t0 = g_ms;
    while (EPD_BUSY()) {
        if ((uint32_t)(g_ms - t0) > 2000u) break;
    }
}

static void epd_write_reg(uint8_t idx, const uint8_t *data, uint16_t len) {
    EPD_CS_LOW();
    spi1_xfer(0x70);
    spi1_xfer(idx);
    EPD_CS_HIGH();
    EPD_CS_LOW();
    spi1_xfer(0x72);
    for (uint16_t i = 0; i < len; i++) spi1_xfer(data[i]);
    EPD_CS_HIGH();
}

static void epd_write_reg1(uint8_t idx, uint8_t val) {
    epd_write_reg(idx, &val, 1);
}

static uint8_t epd_read_reg1(uint8_t idx) {
    EPD_CS_LOW();
    spi1_xfer(0x70);
    spi1_xfer(idx);
    EPD_CS_HIGH();
    EPD_CS_LOW();
    epd_wait_busy();
    spi1_xfer(0x73);
    uint8_t v = spi1_xfer(0x00);
    EPD_CS_HIGH();
    return v;
}

static uint8_t epd_read_id(void) {
    EPD_CS_LOW();
    epd_wait_busy();
    spi1_xfer(0x71);
    uint8_t v = spi1_xfer(0x00);
    EPD_CS_HIGH();
    return v;
}

void epd_init_pins(void) {
    RCC_APB2ENR |= (1u << 12); /* SPI1EN */

    gpio_config(GPIOA, 3, GPIO_MODE_OUT_PP_50); /* BORDER */
    gpio_config(GPIOA, 4, GPIO_MODE_OUT_PP_50); /* DISCHARGE */
    gpio_config(GPIOA, 5, GPIO_MODE_AF_PP_10);  /* SPI1_SCK */
    gpio_config(GPIOA, 6, GPIO_MODE_IN_FLOAT);  /* SPI1_MISO */
    gpio_config(GPIOA, 7, GPIO_MODE_AF_PP_10);  /* SPI1_MOSI */
    gpio_config(GPIOB, 1, GPIO_MODE_OUT_PP_50); /* PANEL_ON */
    gpio_config(GPIOB, 2, GPIO_MODE_OUT_PP_50); /* RESET */
    gpio_config(GPIOB, 10, GPIO_MODE_IN_FLOAT); /* BUSY */
    gpio_config(GPIOB, 11, GPIO_MODE_OUT_PP_50);/* CS */

    EPD_PANEL_OFF();
    EPD_RST_LOW();
    EPD_BORDER_LOW();
    EPD_DISCHARGE_LOW();
    EPD_CS_LOW();

    /* SPI1 Master, BR=/8 (9 MHz), SSM|SSI, 8-bit, CPOL=0 CPHA=0 */
    SPI1->CR1 = (1u << 9) | (1u << 8) | (2u << 3) | (1u << 2) | (1u << 6);
}

static int epd_power_on(void) {
    static const uint8_t chan_sel[8] = { 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xFE, 0x00, 0x00 };

    EPD_PANEL_ON();
    delay_ms(10);
    EPD_CS_HIGH();
    EPD_BORDER_HIGH();
    EPD_RST_HIGH();
    delay_ms(15);
    EPD_RST_LOW();
    delay_ms(15);
    EPD_RST_HIGH();
    delay_ms(15);

    epd_wait_busy();
    uint8_t id = epd_read_id() & 0x0Fu;
    if (id != 1u && id != 2u) return -1;

    epd_write_reg1(0x02, 0x40);
    if ((epd_read_reg1(0x0F) & 0x80u) == 0u) return -2;

    epd_write_reg1(0x0B, 0x02);
    epd_write_reg(0x01, chan_sel, 8);
    epd_write_reg1(0x07, 0xD1);
    epd_write_reg1(0x08, 0x02);
    epd_write_reg1(0x09, 0xC2);
    epd_write_reg1(0x04, 0x03);
    epd_write_reg1(0x03, 0x01);
    epd_write_reg1(0x03, 0x00);
    delay_ms(15);

    for (int tries = 0; tries < 4; tries++) {
        epd_write_reg1(0x05, 0x01);
        delay_ms(250);
        epd_write_reg1(0x05, 0x03);
        delay_ms(50);
        epd_write_reg1(0x05, 0x0F);
        delay_ms(50);
        if (epd_read_reg1(0x0F) & 0x40u) {
            epd_write_reg1(0x02, 0x04);
            return 0;
        }
    }
    return -3;
}

static void epd_send_line(uint8_t line, const uint8_t even[33], const uint8_t odd[33], uint8_t border) {
    uint8_t buf[112];
    buf[0] = 0x72;
    buf[1] = border;
    for (int i = 0; i < 33; i++) buf[2 + i] = even[i];
    for (int i = 0; i < 44; i++) buf[35 + i] = 0x00;
    buf[35 + (line >> 2)] = (uint8_t)(0xC0u >> ((line & 3u) * 2u));
    for (int i = 0; i < 33; i++) buf[79 + i] = odd[i];

    EPD_CS_LOW();
    spi1_xfer(0x70);
    spi1_xfer(0x0A);
    EPD_CS_HIGH();

    EPD_CS_LOW();
    for (int i = 0; i < 112; i++) {
        epd_wait_busy();
        spi1_xfer(buf[i]);
    }
    EPD_CS_HIGH();
    epd_write_reg1(0x02, 0x07); /* Output enable */
}

static void epd_fill_stage(uint8_t pixel_byte, uint32_t stage_ms) {
    uint8_t fill[33];
    for (int i = 0; i < 33; i++) fill[i] = pixel_byte;
    uint32_t t0 = g_ms;
    do {
        for (uint8_t y = 0; y < EPD_HEIGHT; y++)
            epd_send_line(y, fill, fill, 0x00);
    } while ((uint32_t)(g_ms - t0) < stage_ms);
}

static uint8_t epd_encode_even(uint8_t b, uint8_t stage) {
    uint8_t p = b & 0x55u;
    if (stage == 0u)      return (uint8_t)((p ^ 0x55u) | 0xAAu);              /* compensate */
    else if (stage == 1u) return (uint8_t)((p ^ 0x55u) + 0x55u);              /* white */
    else if (stage == 2u) return (uint8_t)((((p ^ 0x55u) << 1) | 0x55u));     /* inverse */
    else                  return (uint8_t)(p | 0xAAu);                        /* normal */
}

static uint8_t epd_encode_odd(uint8_t b, uint8_t stage) {
    uint8_t p = b & 0xAAu;
    uint8_t r;
    if (stage == 0u)      r = (uint8_t)(((p ^ 0xAAu) >> 1) | 0xAAu);          /* compensate */
    else if (stage == 1u) r = (uint8_t)(((p ^ 0xAAu) >> 1) + 0x55u);          /* white */
    else if (stage == 2u) r = (uint8_t)((p ^ 0xAAu) | 0x55u);                 /* inverse */
    else                  r = (uint8_t)((p >> 1) | 0xAAu);                    /* normal */
    /* Reverse the four 2-bit pixel pairs inside the byte (G2 odd-side scan order) */
    return (uint8_t)(((r & 0x03u) << 6) | ((r & 0x0Cu) << 2) |
                     ((r & 0x30u) >> 2) | ((r & 0xC0u) >> 6));
}

static uint8_t rev8(uint8_t b) {
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    return (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
}

/* Match Displio G2 COG scan order, oriented right-side-up (y=0 top, x=0 left) */
static void epd_frame_stage(const uint8_t *fb, uint8_t stage, uint32_t stage_ms) {
    uint8_t even[33], odd[33];
    uint32_t t0 = g_ms;
    do {
        for (uint8_t y = 0; y < EPD_HEIGHT; y++) {
            const uint8_t *row = fb + (uint32_t)y * 33u;
            for (int x = 0; x < 33; x++) {
                even[x] = epd_encode_even(rev8(row[x]), stage);
                odd[x]  = epd_encode_odd(rev8(row[32 - x]), stage);
            }
            epd_send_line(y, even, odd, 0x00);
        }
    } while ((uint32_t)(g_ms - t0) < stage_ms);
}

static void epd_power_off(void) {
    uint8_t zero[33];
    for (int i = 0; i < 33; i++) zero[i] = 0;
    /* Nothing frame */
    for (uint8_t y = 0; y < EPD_HEIGHT; y++)
        epd_send_line(y, zero, zero, 0x00);
    /* Dummy line with border */
    uint8_t buf[112];
    for (int i = 0; i < 112; i++) buf[i] = 0;
    buf[0] = 0x72;
    buf[1] = 0x00;
    EPD_CS_LOW();
    spi1_xfer(0x70); spi1_xfer(0x0A);
    EPD_CS_HIGH();
    EPD_CS_LOW();
    for (int i = 0; i < 112; i++) { epd_wait_busy(); spi1_xfer(buf[i]); }
    EPD_CS_HIGH();
    epd_write_reg1(0x02, 0x07);
    delay_ms(25);

    EPD_BORDER_LOW();
    delay_ms(150);
    EPD_BORDER_HIGH();

    epd_write_reg1(0x0B, 0x00);
    epd_write_reg1(0x03, 0x01);
    epd_write_reg1(0x05, 0x03);
    epd_write_reg1(0x05, 0x01);
    delay_ms(120);
    epd_write_reg1(0x04, 0x80);
    epd_write_reg1(0x05, 0x00);
    epd_write_reg1(0x07, 0x01);
    delay_ms(50);

    EPD_PANEL_OFF();
    EPD_RST_LOW();
    EPD_BORDER_LOW();
    EPD_CS_LOW();
    spi1_xfer(0x00);

    EPD_DISCHARGE_HIGH();
    delay_ms(150);
    EPD_DISCHARGE_LOW();
}

int epd_refresh(const uint8_t *old_fb, const uint8_t *new_fb, uint8_t full_clear) {
    int rc = epd_power_on();
    if (rc != 0) {
        EPD_PANEL_OFF();
        return rc;
    }
    const uint32_t stage_ms = 630u;
    if (full_clear || !old_fb) {
        epd_fill_stage(0xFF, 250); /* black */
        epd_fill_stage(0xAA, 250); /* white */
        epd_fill_stage(0xFF, 250); /* black */
        epd_fill_stage(0xAA, 250); /* white */
    } else {
        epd_frame_stage(old_fb, 0, stage_ms);
        epd_frame_stage(old_fb, 1, stage_ms);
    }
    epd_frame_stage(new_fb, 2, stage_ms);
    epd_frame_stage(new_fb, 3, stage_ms);
    epd_power_off();
    return 0;
}
