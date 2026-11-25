#ifndef BUS_SCAN_H
#define BUS_SCAN_H

#include <stdint.h>

/* Scan I2C1 (remap=0 -> PB6/PB7, remap=1 -> PB8/PB9). Writes up to max_out
 * entries of (addr, whoami_0F, whoami_0D, whoami_00) into out[4*i..]. Returns count. */
uint8_t scan_i2c1(uint8_t remap, uint8_t *out, uint8_t max_out);

/* Read GPIOA/B/C IDR and ADC1 channels 0..9 + Vrefint (11 x u16) */
void scan_gpio_adc(uint16_t gpio_idr[3], uint16_t adc_vals[11]);

/* Probe SPI2 (PB13/PB14/PB15) JEDEC 0x9F on candidate CS pins.
 * Writes (port_pin, b0, b1, b2) entries into out. Returns count. */
uint8_t scan_spi2_jedec(uint8_t *out, uint8_t max_out);

#endif
