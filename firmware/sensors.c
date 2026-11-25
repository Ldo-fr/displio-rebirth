#include "stm32f103.h"
#include "sensors.h"

#define I2C1_SCL 6u
#define I2C1_SDA 7u

static uint8_t g_i2c_ready = 0;

static void i2c_dly(void) {
    for (volatile int i = 0; i < 120; i++) __asm volatile("nop"); /* ~6 us -> ~80 kHz */
}

static void i2c1_ensure_init(void) {
    if (g_i2c_ready) return;
    gpio_set(GPIOB, I2C1_SCL);
    gpio_set(GPIOB, I2C1_SDA);
    gpio_config(GPIOB, I2C1_SCL, 0x7u); /* Out OD 50 MHz */
    gpio_config(GPIOB, I2C1_SDA, 0x7u);
    for (int i = 0; i < 9; i++) {
        gpio_clear(GPIOB, I2C1_SCL); i2c_dly();
        gpio_set(GPIOB, I2C1_SCL);   i2c_dly();
    }
    g_i2c_ready = 1;
}

static void i2c_start(void) {
    gpio_set(GPIOB, I2C1_SDA); gpio_set(GPIOB, I2C1_SCL); i2c_dly();
    gpio_clear(GPIOB, I2C1_SDA); i2c_dly();
    gpio_clear(GPIOB, I2C1_SCL); i2c_dly();
}

static void i2c_stop(void) {
    gpio_clear(GPIOB, I2C1_SDA); i2c_dly();
    gpio_set(GPIOB, I2C1_SCL); i2c_dly();
    gpio_set(GPIOB, I2C1_SDA); i2c_dly();
}

static int i2c_write_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if ((b >> i) & 1u) gpio_set(GPIOB, I2C1_SDA); else gpio_clear(GPIOB, I2C1_SDA);
        i2c_dly();
        gpio_set(GPIOB, I2C1_SCL); i2c_dly();
        gpio_clear(GPIOB, I2C1_SCL); i2c_dly();
    }
    gpio_set(GPIOB, I2C1_SDA); i2c_dly();
    gpio_set(GPIOB, I2C1_SCL); i2c_dly();
    int ack = (gpio_read(GPIOB, I2C1_SDA) == 0);
    gpio_clear(GPIOB, I2C1_SCL); i2c_dly();
    return ack;
}

static uint8_t i2c_read_byte(int ack) {
    uint8_t v = 0;
    gpio_set(GPIOB, I2C1_SDA);
    for (int i = 7; i >= 0; i--) {
        i2c_dly();
        gpio_set(GPIOB, I2C1_SCL); i2c_dly();
        if (gpio_read(GPIOB, I2C1_SDA)) v |= (uint8_t)(1u << i);
        gpio_clear(GPIOB, I2C1_SCL);
    }
    if (ack) gpio_clear(GPIOB, I2C1_SDA); else gpio_set(GPIOB, I2C1_SDA);
    i2c_dly();
    gpio_set(GPIOB, I2C1_SCL); i2c_dly();
    gpio_clear(GPIOB, I2C1_SCL); i2c_dly();
    gpio_set(GPIOB, I2C1_SDA);
    return v;
}

int i2c1_xfer(uint8_t addr, const uint8_t *tx, uint8_t tx_len,
              uint8_t *rx, uint8_t rx_len, uint8_t wait_ms) {
    i2c1_ensure_init();
    if (tx_len > 0) {
        i2c_start();
        if (!i2c_write_byte((uint8_t)(addr << 1))) { i2c_stop(); return -1; }
        for (uint8_t i = 0; i < tx_len; i++) {
            if (!i2c_write_byte(tx[i])) { i2c_stop(); return -2; }
        }
        if (rx_len == 0) {
            i2c_stop();
            return 0;
        }
        if (wait_ms > 0) {
            i2c_stop();
            delay_ms(wait_ms);
        }
    }
    if (rx_len > 0) {
        i2c_start();
        if (!i2c_write_byte((uint8_t)((addr << 1) | 1u))) { i2c_stop(); return -3; }
        for (uint8_t i = 0; i < rx_len; i++) {
            rx[i] = i2c_read_byte(i + 1u < rx_len);
        }
        i2c_stop();
    }
    return 0;
}

/* MMA8452Q at I2C 0x1C */
#define MMA_ADDR 0x1Cu

int mma8452_init(void) {
    uint8_t reg = 0x0Du, who = 0;
    if (i2c1_xfer(MMA_ADDR, &reg, 1, &who, 1, 0) != 0 || who != 0x2Au) return -1;

    /* Put in standby (CTRL_REG1 0x2A = 0x00) */
    uint8_t cmd1[2] = { 0x2Au, 0x00u };
    i2c1_xfer(MMA_ADDR, cmd1, 2, 0, 0, 0);

    /* +/-2g range (XYZ_DATA_CFG 0x0E = 0x00) */
    uint8_t cmd2[2] = { 0x0Eu, 0x00u };
    i2c1_xfer(MMA_ADDR, cmd2, 2, 0, 0, 0);

    /* Enable portrait/landscape detection (PL_CFG 0x11 = 0x40) */
    uint8_t cmd3[2] = { 0x11u, 0x40u };
    i2c1_xfer(MMA_ADDR, cmd3, 2, 0, 0, 0);

    /* Active mode, 100 Hz ODR (CTRL_REG1 0x2A = 0x19) */
    uint8_t cmd4[2] = { 0x2Au, 0x19u };
    return i2c1_xfer(MMA_ADDR, cmd4, 2, 0, 0, 0);
}

int mma8452_read(int16_t *x_mg, int16_t *y_mg, int16_t *z_mg, uint8_t *pl_status) {
    uint8_t reg = 0x01u;
    uint8_t raw[6];
    if (i2c1_xfer(MMA_ADDR, &reg, 1, raw, 6, 0) != 0) return -1;

    /* 12-bit signed left-justified in 16-bit big-endian; 1024 counts = 1000 mg at +/-2g */
    int16_t rx = (int16_t)((raw[0] << 8) | raw[1]) >> 4;
    int16_t ry = (int16_t)((raw[2] << 8) | raw[3]) >> 4;
    int16_t rz = (int16_t)((raw[4] << 8) | raw[5]) >> 4;
    *x_mg = (int16_t)((rx * 1000) / 1024);
    *y_mg = (int16_t)((ry * 1000) / 1024);
    *z_mg = (int16_t)((rz * 1000) / 1024);

    uint8_t pl_reg = 0x10u;
    uint8_t pl = 0;
    if (i2c1_xfer(MMA_ADDR, &pl_reg, 1, &pl, 1, 0) == 0) *pl_status = pl;
    return 0;
}

static uint32_t g_spi2_cr1 = 0;

void spi2_init(uint8_t mode, uint8_t br_div) {
    uint32_t cr1 = (1u << 9) | (1u << 8) | (((uint32_t)(br_div & 7u)) << 3) |
                   (1u << 2) | (mode & 3u) | (1u << 6);
    if (g_spi2_cr1 == cr1) return;
    RCC_APB1ENR |= (1u << 14); /* SPI2EN */
    gpio_config(GPIOB, 13, GPIO_MODE_AF_PP_10); /* SCK */
    gpio_config(GPIOB, 14, GPIO_MODE_IN_FLOAT); /* MISO */
    gpio_config(GPIOB, 15, GPIO_MODE_AF_PP_10); /* MOSI */
    SPI2->CR1 = cr1;
    g_spi2_cr1 = cr1;
}

void spi2_xfer_buf(uint8_t cs_port_pin, const uint8_t *tx, uint8_t *rx, uint8_t len) {
    GPIO_TypeDef *ports[3] = { GPIOA, GPIOB, GPIOC };
    GPIO_TypeDef *cs_p = 0;
    uint8_t cs_pin = 0;
    if (cs_port_pin != 0xFFu && (cs_port_pin >> 4) < 3u) {
        cs_p = ports[cs_port_pin >> 4];
        cs_pin = cs_port_pin & 0x0Fu;
        gpio_config(cs_p, cs_pin, GPIO_MODE_OUT_PP_50);
        gpio_clear(cs_p, cs_pin);
    }
    /* Drain any stale RXNE before starting */
    while (SPI2->SR & (1u << 0)) (void)SPI2->DR;
    for (uint8_t i = 0; i < len; i++) {
        while (!(SPI2->SR & (1u << 1))) {} /* wait TXE */
        SPI2->DR = tx ? tx[i] : 0x00u;
        while (!(SPI2->SR & (1u << 0))) {} /* wait RXNE */
        uint8_t b = (uint8_t)SPI2->DR;
        if (rx) rx[i] = b;
    }
    while (SPI2->SR & (1u << 7)) {} /* wait BSY=0 before raising CS */
    if (cs_p) gpio_set(cs_p, cs_pin);
}
