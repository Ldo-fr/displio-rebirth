#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>

void usb_hid_init(void);
void usb_hid_poll(void);
int  usb_hid_rx_ready(void);
void usb_hid_read(uint8_t out[64]);
int  usb_hid_send(const uint8_t in[64]);

#endif
