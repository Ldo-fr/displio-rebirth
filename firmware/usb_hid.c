/*
 * Minimal, self-contained STM32F103 USB Full-Speed HID driver (64B IN / 64B OUT).
 * Uses VID:PID 0483:A197 so existing udev rules work seamlessly.
 * USB D+ pull-up is on PA10 (active LOW).
 */
#include "stm32f103.h"
#include "usb_hid.h"

/* STM32F103 USB PMA layout (offsets in USB 16-bit address space) */
#define BTABLE_ADDR     0x0000u
#define EP0_TX_ADDR     0x0040u
#define EP0_RX_ADDR     0x0080u
#define EP1_TX_ADDR     0x00C0u
#define EP1_RX_ADDR     0x0100u

/* BTABLE entries (each entry is 4 x 16-bit words stored at 32-bit stride in APB space) */
#define BTABLE_TX_ADDR(ep)  REG32(USB_PMA_BASE + ((ep) * 16u) + 0u)
#define BTABLE_TX_CNT(ep)   REG32(USB_PMA_BASE + ((ep) * 16u) + 4u)
#define BTABLE_RX_ADDR(ep)  REG32(USB_PMA_BASE + ((ep) * 16u) + 8u)
#define BTABLE_RX_CNT(ep)   REG32(USB_PMA_BASE + ((ep) * 16u) + 12u)

/* EPnR bits */
#define EP_CTR_RX       (1u << 15)
#define EP_DTOG_RX      (1u << 14)
#define EP_STAT_RX_MASK (3u << 12)
#define EP_SETUP        (1u << 11)
#define EP_TYPE_MASK    (3u << 9)
#define EP_TYPE_CTRL    (1u << 9)
#define EP_TYPE_INT     (3u << 9)
#define EP_CTR_TX       (1u << 7)
#define EP_DTOG_TX      (1u << 6)
#define EP_STAT_TX_MASK (3u << 4)
#define EP_EA_MASK      0x000Fu
#define EP_RW_MASK      (EP_TYPE_MASK | (1u << 8) | EP_EA_MASK)

#define STAT_DIS        0u
#define STAT_STALL      1u
#define STAT_NAK        2u
#define STAT_VALID      3u

static inline void ep_set_stat_tx(uint8_t ep, uint16_t stat) {
    uint16_t v = (uint16_t)USB_EP(ep);
    uint16_t toggle = (v ^ (stat << 4)) & EP_STAT_TX_MASK;
    USB_EP(ep) = (v & EP_RW_MASK) | EP_CTR_RX | EP_CTR_TX | toggle;
}

static inline void ep_set_stat_rx(uint8_t ep, uint16_t stat) {
    uint16_t v = (uint16_t)USB_EP(ep);
    uint16_t toggle = (v ^ (stat << 12)) & EP_STAT_RX_MASK;
    USB_EP(ep) = (v & EP_RW_MASK) | EP_CTR_RX | EP_CTR_TX | toggle;
}

static inline void ep_clear_ctr_rx(uint8_t ep) {
    uint16_t v = (uint16_t)USB_EP(ep);
    USB_EP(ep) = (v & EP_RW_MASK) | EP_CTR_TX;
}

static inline void ep_clear_ctr_tx(uint8_t ep) {
    uint16_t v = (uint16_t)USB_EP(ep);
    USB_EP(ep) = (v & EP_RW_MASK) | EP_CTR_RX;
}

static void pma_write(uint16_t pma_addr, const uint8_t *src, uint16_t len) {
    volatile uint32_t *dst = (volatile uint32_t *)(USB_PMA_BASE + (uint32_t)pma_addr * 2u);
    uint16_t words = (len + 1u) >> 1;
    for (uint16_t i = 0; i < words; i++) {
        uint16_t lo = src[i * 2u];
        uint16_t hi = (i * 2u + 1u < len) ? src[i * 2u + 1u] : 0u;
        dst[i] = lo | ((uint32_t)hi << 8);
    }
}

static void pma_read(uint16_t pma_addr, uint8_t *dst, uint16_t len) {
    volatile uint32_t *src = (volatile uint32_t *)(USB_PMA_BASE + (uint32_t)pma_addr * 2u);
    uint16_t words = (len + 1u) >> 1;
    for (uint16_t i = 0; i < words; i++) {
        uint16_t w = (uint16_t)src[i];
        dst[i * 2u] = (uint8_t)(w & 0xFFu);
        if (i * 2u + 1u < len)
            dst[i * 2u + 1u] = (uint8_t)(w >> 8);
    }
}

/* USB Descriptors */
static const uint8_t hid_report_desc[28] = {
    0x06, 0x00, 0xFF, /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x01,       /* Usage (0x01) */
    0xA1, 0x01,       /* Collection (Application) */
    0x19, 0x00,       /* Usage Min (0) */
    0x29, 0x40,       /* Usage Max (64) */
    0x15, 0x00,       /* Logical Min (0) */
    0x25, 0x40,       /* Logical Max (64) */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x40,       /* Report Count (64) */
    0x81, 0x00,       /* Input (Data, Array, Abs) */
    0x19, 0x00,       /* Usage Min (0) */
    0x29, 0x40,       /* Usage Max (64) */
    0x91, 0x00,       /* Output (Data, Array, Abs) */
    0xC0              /* End Collection */
};

static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    0x83, 0x04,       /* VID 0x0483 */
    0x97, 0xA1,       /* PID 0xA197 */
    0x00, 0x02,       /* bcdDevice 2.00 (custom fw!) */
    1, 2, 0, 1
};

static const uint8_t cfg_desc[41] = {
    /* Config */
    9, 2, 41, 0, 1, 1, 0, 0xC0, 50,
    /* Interface 0: HID */
    9, 4, 0, 0, 2, 3, 0, 0, 0,
    /* HID */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(hid_report_desc), 0,
    /* EP1 IN */
    7, 5, 0x81, 3, 64, 0, 1,
    /* EP1 OUT */
    7, 5, 0x01, 3, 64, 0, 1
};

static const uint8_t str0_desc[] = { 4, 3, 0x09, 0x04 };
static const uint8_t str1_desc[] = {
    16, 3, 'D',0,'I',0,'S',0,'P',0,'L',0,'I',0,'O',0
};
static const uint8_t str2_desc[] = {
    32, 3, 'D',0,'I',0,'S',0,'P',0,'L',0,'I',0,'O',0,' ',0,'R',0,'E',0,'B',0,'I',0,'R',0,'T',0,'H',0
};

static uint8_t  g_pending_addr = 0;
static uint8_t  g_config = 0;
static const uint8_t *g_tx_ptr = 0;
static uint16_t g_tx_rem = 0;

static volatile uint8_t g_rx_ready = 0;
static volatile uint8_t g_tx_busy  = 0;
static uint8_t  g_rx_buf[64];

static void ep0_send_chunk(void) {
    uint16_t n = g_tx_rem > 64u ? 64u : g_tx_rem;
    if (n > 0) {
        pma_write(EP0_TX_ADDR, g_tx_ptr, n);
        g_tx_ptr += n;
        g_tx_rem -= n;
    }
    BTABLE_TX_CNT(0) = n;
    ep_set_stat_tx(0, STAT_VALID);
}

static void handle_setup(void) {
    uint8_t s[8];
    pma_read(EP0_RX_ADDR, s, 8);
    uint8_t  bmReq = s[0];
    uint8_t  bReq  = s[1];
    uint16_t wVal  = (uint16_t)(s[2] | (s[3] << 8));
    uint16_t wLen  = (uint16_t)(s[6] | (s[7] << 8));

    g_tx_ptr = 0;
    g_tx_rem = 0;

    if ((bmReq & 0x60u) == 0x00u) { /* Standard request */
        if (bReq == 0x06u) { /* GET_DESCRIPTOR */
            const uint8_t *d = 0;
            uint16_t l = 0;
            uint8_t dtype = (uint8_t)(wVal >> 8);
            uint8_t didx  = (uint8_t)(wVal & 0xFFu);
            if (dtype == 1u) { d = dev_desc; l = sizeof(dev_desc); }
            else if (dtype == 2u) { d = cfg_desc; l = sizeof(cfg_desc); }
            else if (dtype == 3u) {
                if (didx == 0u)      { d = str0_desc; l = sizeof(str0_desc); }
                else if (didx == 1u) { d = str1_desc; l = sizeof(str1_desc); }
                else if (didx == 2u) { d = str2_desc; l = sizeof(str2_desc); }
            } else if (dtype == 0x22u) { d = hid_report_desc; l = sizeof(hid_report_desc); }
            if (d) {
                g_tx_ptr = d;
                g_tx_rem = (l < wLen) ? l : wLen;
                ep0_send_chunk();
                ep_set_stat_rx(0, STAT_VALID);
                return;
            }
        } else if (bReq == 0x05u) { /* SET_ADDRESS */
            g_pending_addr = (uint8_t)(wVal & 0x7Fu);
            BTABLE_TX_CNT(0) = 0;
            ep_set_stat_tx(0, STAT_VALID);
            ep_set_stat_rx(0, STAT_VALID);
            return;
        } else if (bReq == 0x09u) { /* SET_CONFIGURATION */
            g_config = (uint8_t)(wVal & 0xFFu);
            BTABLE_TX_CNT(0) = 0;
            ep_set_stat_tx(0, STAT_VALID);
            ep_set_stat_rx(0, STAT_VALID);
            return;
        } else if (bReq == 0x08u) { /* GET_CONFIGURATION */
            static uint8_t c;
            c = g_config;
            g_tx_ptr = &c;
            g_tx_rem = 1;
            ep0_send_chunk();
            ep_set_stat_rx(0, STAT_VALID);
            return;
        } else if (bReq == 0x00u) { /* GET_STATUS */
            static const uint8_t z[2] = { 0, 0 };
            g_tx_ptr = z;
            g_tx_rem = 2;
            ep0_send_chunk();
            ep_set_stat_rx(0, STAT_VALID);
            return;
        }
    } else if ((bmReq & 0x60u) == 0x20u) { /* HID Class request */
        if (bReq == 0x0Au || bReq == 0x0Bu) { /* SET_IDLE / SET_PROTOCOL */
            BTABLE_TX_CNT(0) = 0;
            ep_set_stat_tx(0, STAT_VALID);
            ep_set_stat_rx(0, STAT_VALID);
            return;
        }
    }

    ep_set_stat_tx(0, STAT_STALL);
    ep_set_stat_rx(0, STAT_VALID);
}

static void usb_reset(void) {
    USB_BTABLE = BTABLE_ADDR;

    /* EP0 Control */
    BTABLE_TX_ADDR(0) = EP0_TX_ADDR;
    BTABLE_TX_CNT(0)  = 0;
    BTABLE_RX_ADDR(0) = EP0_RX_ADDR;
    BTABLE_RX_CNT(0)  = (1u << 15) | (1u << 10); /* 64 bytes (BL_SIZE=1, NUM_BLOCK=1) */
    USB_EP(0) = EP_TYPE_CTRL | 0u;
    ep_set_stat_rx(0, STAT_VALID);
    ep_set_stat_tx(0, STAT_NAK);

    /* EP1 Interrupt IN/OUT */
    BTABLE_TX_ADDR(1) = EP1_TX_ADDR;
    BTABLE_TX_CNT(1)  = 0;
    BTABLE_RX_ADDR(1) = EP1_RX_ADDR;
    BTABLE_RX_CNT(1)  = (1u << 15) | (1u << 10); /* 64 bytes */
    USB_EP(1) = EP_TYPE_INT | 1u;
    ep_set_stat_rx(1, STAT_VALID);
    ep_set_stat_tx(1, STAT_NAK);

    g_pending_addr = 0;
    g_config = 0;
    g_tx_busy = 0;
    g_rx_ready = 0;
    USB_DADDR = 0x80u; /* EF = 1, ADD = 0 */
}

void USB_LP_IRQHandler(void) {
    uint16_t istr = (uint16_t)USB_ISTR;
    if (istr & (1u << 10)) { /* RESET */
        USB_ISTR = ~(1u << 10);
        usb_reset();
    }
    while ((istr = (uint16_t)USB_ISTR) & (1u << 15)) { /* CTR */
        uint8_t ep = (uint8_t)(istr & 0x0Fu);
        uint16_t epr = (uint16_t)USB_EP(ep);
        if (ep == 0u) {
            if (epr & EP_CTR_RX) {
                ep_clear_ctr_rx(0);
                if (epr & EP_SETUP) {
                    handle_setup();
                } else {
                    ep_set_stat_rx(0, STAT_VALID);
                }
            }
            if (epr & EP_CTR_TX) {
                ep_clear_ctr_tx(0);
                if (g_pending_addr) {
                    USB_DADDR = 0x80u | g_pending_addr;
                    g_pending_addr = 0;
                } else if (g_tx_rem > 0) {
                    ep0_send_chunk();
                }
            }
        } else if (ep == 1u) {
            if (epr & EP_CTR_RX) {
                ep_clear_ctr_rx(1);
                pma_read(EP1_RX_ADDR, g_rx_buf, 64);
                g_rx_ready = 1;
                /* Keep EP1 RX NAK until app reads buffer */
            }
            if (epr & EP_CTR_TX) {
                ep_clear_ctr_tx(1);
                g_tx_busy = 0;
            }
        }
    }
}

void usb_hid_init(void) {
    /* Disconnect D+ pull-up (PA10 HIGH) first for clean host re-enumeration */
    gpio_config(GPIOA, 10, GPIO_MODE_OUT_PP_50);
    gpio_set(GPIOA, 10);

    RCC_APB1ENR |= (1u << 23); /* USBEN */
    delay_ms(30);

    /* Power up USB analog transceiver, hold FRES, then release */
    USB_CNTR = 1u; /* FRES=1, PDWN=0 */
    delay_ms(2);
    USB_CNTR = 0u; /* FRES=0 */
    USB_ISTR = 0u;
    USB_CNTR = (1u << 15) | (1u << 10); /* CTRM | RESETM */

    NVIC_ISER0 = (1u << 20); /* Enable USB_LP_CAN1_RX0 IRQ */

    /* Pull PA10 LOW to assert USB D+ 1.5k pull-up */
    gpio_clear(GPIOA, 10);
}

int usb_hid_rx_ready(void) {
    return g_rx_ready;
}

void usb_hid_read(uint8_t out[64]) {
    for (int i = 0; i < 64; i++) out[i] = g_rx_buf[i];
    g_rx_ready = 0;
    ep_set_stat_rx(1, STAT_VALID);
}

int usb_hid_send(const uint8_t in[64]) {
    uint32_t t0 = g_ms;
    while (g_tx_busy) {
        if ((uint32_t)(g_ms - t0) > 500u) return -1;
    }
    g_tx_busy = 1;
    pma_write(EP1_TX_ADDR, in, 64);
    BTABLE_TX_CNT(1) = 64;
    ep_set_stat_tx(1, STAT_VALID);
    return 0;
}
