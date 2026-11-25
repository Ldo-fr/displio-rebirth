#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>

/* Generic bit-banged I2C1 (PB6=SCL, PB7=SDA) transaction:
 * Writes tx_len bytes, optionally waits delay_ms, then reads rx_len bytes (with repeated start if delay==0).
 * Returns 0 on ACK, negative on NACK. */
int i2c1_xfer(uint8_t addr, const uint8_t *tx, uint8_t tx_len,
              uint8_t *rx, uint8_t rx_len, uint8_t wait_ms);

/* MMA8452Q (I2C1 0x1C) accelerometer */
int mma8452_init(void);
int mma8452_read(int16_t *x_mg, int16_t *y_mg, int16_t *z_mg, uint8_t *pl_status);

/* Generic SPI2 (PB13=SCK, PB14=MISO, PB15=MOSI) transfer with selectable CS pin
 * cs_port_pin: (port_idx<<4)|pin, or 0xFF for no CS toggle; mode: CPOL<<1 | CPHA */
void spi2_init(uint8_t mode, uint8_t br_div);
void spi2_xfer_buf(uint8_t cs_port_pin, const uint8_t *tx, uint8_t *rx, uint8_t len);

#endif
