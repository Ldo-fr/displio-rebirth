#include "stm32f103.h"
#include "bus_scan.h"

/* Bit-banged I2C master on any two GPIOB pins (immune to STM32F1 I2C peripheral lockups!) */
static void i2c_delay(void) {
    for (volatile int i = 0; i < 180; i++) __asm volatile("nop"); /* ~10 us -> ~50 kHz */
}

static void bb_i2c_init(uint8_t scl, uint8_t sda) {
    gpio_set(GPIOB, scl);
    gpio_set(GPIOB, sda);
    gpio_config(GPIOB, scl, 0x7u); /* Out OD 50 MHz */
    gpio_config(GPIOB, sda, 0x7u); /* Out OD 50 MHz */
    /* Clock out 9 pulses in case a slave is stuck holding SDA low */
    for (int i = 0; i < 9; i++) {
        gpio_clear(GPIOB, scl); i2c_delay();
        gpio_set(GPIOB, scl);   i2c_delay();
    }
}

static void bb_start(uint8_t scl, uint8_t sda) {
    gpio_set(GPIOB, sda); gpio_set(GPIOB, scl); i2c_delay();
    gpio_clear(GPIOB, sda); i2c_delay();
    gpio_clear(GPIOB, scl); i2c_delay();
}

static void bb_stop(uint8_t scl, uint8_t sda) {
    gpio_clear(GPIOB, sda); i2c_delay();
    gpio_set(GPIOB, scl); i2c_delay();
    gpio_set(GPIOB, sda); i2c_delay();
}

static int bb_write_byte(uint8_t scl, uint8_t sda, uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if ((b >> i) & 1u) gpio_set(GPIOB, sda); else gpio_clear(GPIOB, sda);
        i2c_delay();
        gpio_set(GPIOB, scl); i2c_delay();
        gpio_clear(GPIOB, scl); i2c_delay();
    }
    gpio_set(GPIOB, sda); i2c_delay();
    gpio_set(GPIOB, scl); i2c_delay();
    int ack = (gpio_read(GPIOB, sda) == 0);
    gpio_clear(GPIOB, scl); i2c_delay();
    return ack;
}

static uint8_t bb_read_byte(uint8_t scl, uint8_t sda, int ack) {
    uint8_t v = 0;
    gpio_set(GPIOB, sda);
    for (int i = 7; i >= 0; i--) {
        i2c_delay();
        gpio_set(GPIOB, scl); i2c_delay();
        if (gpio_read(GPIOB, sda)) v |= (uint8_t)(1u << i);
        gpio_clear(GPIOB, scl);
    }
    if (ack) gpio_clear(GPIOB, sda); else gpio_set(GPIOB, sda);
    i2c_delay();
    gpio_set(GPIOB, scl); i2c_delay();
    gpio_clear(GPIOB, scl); i2c_delay();
    gpio_set(GPIOB, sda);
    return v;
}

static uint8_t bb_read_reg(uint8_t scl, uint8_t sda, uint8_t addr, uint8_t reg) {
    bb_start(scl, sda);
    if (!bb_write_byte(scl, sda, (uint8_t)(addr << 1))) { bb_stop(scl, sda); return 0xFF; }
    if (!bb_write_byte(scl, sda, reg)) { bb_stop(scl, sda); return 0xFF; }
    bb_start(scl, sda);
    if (!bb_write_byte(scl, sda, (uint8_t)((addr << 1) | 1u))) { bb_stop(scl, sda); return 0xFF; }
    uint8_t v = bb_read_byte(scl, sda, 0);
    bb_stop(scl, sda);
    return v;
}

uint8_t scan_i2c1(uint8_t remap, uint8_t *out, uint8_t max_out) {
    uint8_t scl = remap ? 8u : 6u;
    uint8_t sda = remap ? 9u : 7u;
    bb_i2c_init(scl, sda);

    /* If bus lines are stuck LOW (no external pull-ups), abort cleanly */
    if (!gpio_read(GPIOB, scl) || !gpio_read(GPIOB, sda)) {
        gpio_config(GPIOB, scl, GPIO_MODE_IN_FLOAT);
        gpio_config(GPIOB, sda, GPIO_MODE_IN_FLOAT);
        return 0;
    }

    uint8_t count = 0;
    for (uint8_t addr = 0x08u; addr <= 0x77u && count < max_out; addr++) {
        bb_start(scl, sda);
        int ack = bb_write_byte(scl, sda, (uint8_t)(addr << 1));
        bb_stop(scl, sda);
        if (ack) {
            out[count * 4u + 0u] = addr;
            out[count * 4u + 1u] = bb_read_reg(scl, sda, addr, 0x0Fu); /* LIS3DH / ST WHO_AM_I */
            out[count * 4u + 2u] = bb_read_reg(scl, sda, addr, 0x0Du); /* MMA8452Q WHO_AM_I */
            out[count * 4u + 3u] = bb_read_reg(scl, sda, addr, 0x00u); /* ADXL345 / BMA250 */
            count++;
        }
    }
    return count;
}

static uint16_t adc1_read_ch(uint8_t ch) {
    ADC1->SQR1 = 0;
    ADC1->SQR3 = ch;
    ADC1->CR2 |= (1u << 0); /* ADON start conversion */
    uint32_t t0 = g_ms;
    while (!(ADC1->SR & (1u << 1))) {
        if ((uint32_t)(g_ms - t0) > 50u) return 0xFFFFu;
    }
    return (uint16_t)ADC1_DR;
}

void scan_gpio_adc(uint16_t gpio_idr[3], uint16_t adc_vals[11]) {
    gpio_idr[0] = (uint16_t)GPIOA->IDR;
    gpio_idr[1] = (uint16_t)GPIOB->IDR;
    gpio_idr[2] = (uint16_t)GPIOC->IDR;

    /* Enable ADC1 (ADCPRE = /6 -> 12 MHz), TSVREFE for Vrefint (ch17) */
    RCC_CFGR = (RCC_CFGR & ~(3u << 14)) | (2u << 14);
    RCC_APB2ENR |= (1u << 9); /* ADC1EN */
    ADC1->SMPR1 = 0x00FFFFFFu; /* max sample time */
    ADC1->SMPR2 = 0x3FFFFFFFu;
    ADC1->CR2 = (1u << 23) | (1u << 0); /* TSVREFE | ADON */
    delay_ms(2);
    ADC1->CR2 |= (1u << 2); /* CAL */
    while (ADC1->CR2 & (1u << 2)) {}

    (void)adc1_read_ch(0u); /* discard first post-calibration conversion */
    for (uint8_t ch = 0; ch < 10u; ch++) adc_vals[ch] = adc1_read_ch(ch);
    adc_vals[10] = adc1_read_ch(17u); /* Vrefint (~1.20V) */
}

static uint8_t spi2_xfer(uint8_t b) {
    SPI2->DR = b;
    while (SPI2->SR & (1u << 7)) {}
    return (uint8_t)SPI2->DR;
}

uint8_t scan_spi2_jedec(uint8_t *out, uint8_t max_out) {
    RCC_APB1ENR |= (1u << 14); /* SPI2EN */
    gpio_config(GPIOB, 13, GPIO_MODE_AF_PP_10); /* SPI2_SCK */
    gpio_config(GPIOB, 14, GPIO_MODE_IN_FLOAT); /* SPI2_MISO */
    gpio_config(GPIOB, 15, GPIO_MODE_AF_PP_10); /* SPI2_MOSI */
    SPI2->CR1 = (1u << 9) | (1u << 8) | (3u << 3) | (1u << 2) | (1u << 6); /* Master /16 */

    /* Candidate CS pins: (port_idx 0=A,1=B,2=C, pin) */
    static const uint8_t cands[][2] = {
        {1, 12}, {1, 4}, {2, 13}, {1, 3}, {1, 5}, {1, 8}, {1, 9}, {0, 15}, {0, 8}
    };
    GPIO_TypeDef *ports[3] = { GPIOA, GPIOB, GPIOC };
    uint8_t count = 0;

    for (uint8_t i = 0; i < sizeof(cands) / 2u && count < max_out; i++) {
        GPIO_TypeDef *p = ports[cands[i][0]];
        uint8_t pin = cands[i][1];
        gpio_config(p, pin, GPIO_MODE_OUT_PP_50);
        gpio_set(p, pin);
        delay_ms(1);
        gpio_clear(p, pin);
        spi2_xfer(0x9Fu); /* JEDEC RDID */
        uint8_t id0 = spi2_xfer(0x00u);
        uint8_t id1 = spi2_xfer(0x00u);
        uint8_t id2 = spi2_xfer(0x00u);
        gpio_set(p, pin);
        if (!((id0 == 0x00u && id1 == 0x00u && id2 == 0x00u) ||
              (id0 == 0xFFu && id1 == 0xFFu && id2 == 0xFFu))) {
            out[count * 4u + 0u] = (uint8_t)((cands[i][0] << 4) | pin);
            out[count * 4u + 1u] = id0;
            out[count * 4u + 2u] = id1;
            out[count * 4u + 3u] = id2;
            count++;
        }
    }
    return count;
}
