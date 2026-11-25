/*
 * Bare-metal Texas Instruments CC3000 Wi-Fi driver for Displio V1.0 (STM32F103xB)
 * Pinout:
 *   PB12 = VBAT_SW_EN (active HIGH)
 *   PB4  = SPI_CS     (active LOW)
 *   PA8  = SPI_IRQ    (active LOW)
 *   SPI2 = PB13(SCK), PB14(MISO), PB15(MOSI), SPI Mode 1 (CPOL=0, CPHA=1)
 */
#include "stm32f103.h"
#include "sensors.h"
#include "cc3000.h"

#define CC_EN_HIGH()  gpio_set(GPIOB, 12)
#define CC_EN_LOW()   gpio_clear(GPIOB, 12)
#define CC_CS_HIGH()  gpio_set(GPIOB, 4)
#define CC_CS_LOW()   gpio_clear(GPIOB, 4)
#define CC_IRQ()      gpio_read(GPIOA, 8)

/* HCI packet types */
#define HCI_TYPE_CMND 0x01u
#define HCI_TYPE_DATA 0x02u
#define HCI_TYPE_EVNT 0x04u

/* HCI command opcodes */
#define OP_WLAN_CONNECT            0x0001u
#define OP_WLAN_DISCONNECT         0x0002u
#define OP_WLAN_SET_CONN_POLICY    0x0004u
#define OP_WLAN_SET_EVENT_MASK     0x0008u
#define OP_WLAN_IOCTL_STATUSGET    0x0009u
#define OP_SOCKET                  0x1001u
#define OP_BIND                    0x1002u
#define OP_RECV                    0x1004u
#define OP_ACCEPT                  0x1005u
#define OP_LISTEN                  0x1006u
#define OP_SELECT                  0x1008u
#define OP_SETSOCKOPT              0x1009u
#define OP_CLOSE_SOCKET            0x100Bu
#define OP_NETAPP_DHCP             0x2001u
#define OP_NETAPP_PING_SEND        0x2002u
#define OP_NETAPP_IPCONFIG         0x2005u
#define OP_NETAPP_TIMEOUT_VALUES   0x2009u
#define OP_SIMPLE_LINK_START       0x4000u
#define OP_READ_SP_VERSION         0x4007u
#define OP_READ_BUFFER_SIZE        0x400Bu

/* HCI data opcodes */
#define OP_DATA_SEND               0x82u
#define OP_DATA_RECV               0x85u

/* HCI async event opcodes */
#define EV_UNSOL_CONNECT           0x8001u
#define EV_UNSOL_DISCONNECT        0x8002u
#define EV_UNSOL_DHCP              0x8010u
#define EV_UNSOL_KEEPALIVE         0x8200u
#define EV_UNSOL_PING_REPORT       0x8400u
#define EV_UNSOL_BSD_CLOSE_WAIT    0x8800u
#define EV_UNSOL_FREE_BUFF         0x4100u

cc3000_t g_cc;

static uint8_t g_rx[300];
static uint8_t g_tx[540];
static uint16_t g_rx_len = 0;

static void udelay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 14u; i++) __asm volatile("nop");
}

static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int wait_irq(uint8_t target, uint32_t timeout_ms) {
    uint32_t t0 = g_ms;
    while (CC_IRQ() != target) {
        if ((uint32_t)(g_ms - t0) > timeout_ms) return -1;
    }
    return 0;
}

static uint8_t spi2_byte(uint8_t b) {
    while (!(SPI2->SR & (1u << 1))) {}
    SPI2->DR = b;
    while (!(SPI2->SR & (1u << 0))) {}
    return (uint8_t)SPI2->DR;
}

static void spi2_drain(void) {
    while (SPI2->SR & (1u << 0)) (void)SPI2->DR;
}

/* Read one SPI frame when IRQ == 0. Returns payload length in g_rx[], or <= 0 on error. */
static int cc_spi_read(void) {
    if (CC_IRQ() != 0u) return 0;
    spi2_drain();
    CC_CS_LOW();
    udelay(60);
    uint8_t hdr[5];
    hdr[0] = spi2_byte(0x03u);
    hdr[1] = spi2_byte(0x00u);
    hdr[2] = spi2_byte(0x00u);
    hdr[3] = spi2_byte(0x00u);
    hdr[4] = spi2_byte(0x00u);
    uint16_t len = (uint16_t)(((uint16_t)hdr[3] << 8) | hdr[4]);
    /* CC3000 pads SPI frames so total (5 + len) is even -> if len is even, 1 extra pad byte */
    uint16_t wire_len = (len & 1u) ? len : (uint16_t)(len + 1u);
    uint16_t store = (len < sizeof(g_rx)) ? len : (uint16_t)sizeof(g_rx);
    for (uint16_t i = 0; i < wire_len; i++) {
        uint8_t b = spi2_byte(0x00u);
        if (i < store) g_rx[i] = b;
    }
    while (SPI2->SR & (1u << 7)) {}
    CC_CS_HIGH();
    wait_irq(1u, 50u);
    g_rx_len = store;
    return (int)store;
}

/* Write one SPI frame (hci_payload of hci_len bytes). */
static int cc_spi_write(const uint8_t *hci, uint16_t hci_len, int is_first) {
    uint16_t pad = (hci_len & 1u) ? 0u : 1u;
    uint16_t wire_payload = (uint16_t)(hci_len + pad);
    uint16_t total = (uint16_t)(5u + wire_payload);
    if (total > sizeof(g_tx)) return -1;

    g_tx[0] = 0x01u; /* WRITE */
    g_tx[1] = (uint8_t)(wire_payload >> 8);
    g_tx[2] = (uint8_t)(wire_payload & 0xFFu);
    g_tx[3] = 0x00u;
    g_tx[4] = 0x00u;
    for (uint16_t i = 0; i < hci_len; i++) g_tx[5u + i] = hci[i];
    if (pad) g_tx[5u + hci_len] = 0x00u;

    spi2_drain();
    CC_CS_LOW();
    if (is_first) {
        udelay(60);
        for (uint16_t i = 0; i < 4u; i++) spi2_byte(g_tx[i]);
        udelay(60);
        for (uint16_t i = 4u; i < total; i++) spi2_byte(g_tx[i]);
    } else {
        if (wait_irq(0u, 200u) != 0) {
            CC_CS_HIGH();
            return -2;
        }
        for (uint16_t i = 0; i < total; i++) spi2_byte(g_tx[i]);
    }
    while (SPI2->SR & (1u << 7)) {}
    CC_CS_HIGH();
    wait_irq(1u, 100u);
    return 0;
}

static void handle_async_event(void) {
    if (g_rx_len < 4u || g_rx[0] != HCI_TYPE_EVNT) return;
    uint16_t op = get_u16le(g_rx + 1);
    if (op == EV_UNSOL_CONNECT) {
        g_cc.state = CC_STATE_CONNECTED;
    } else if (op == EV_UNSOL_DISCONNECT) {
        g_cc.state = CC_STATE_INIT;
        g_cc.listen_sd = -1;
        for (int i = 0; i < 4; i++) g_cc.ip[i] = 0;
    } else if (op == EV_UNSOL_DHCP && g_rx_len >= 21u) {
        /* CC3000 sends IP in reverse byte order (LSB first) in EV_UNSOL_DHCP */
        g_cc.ip[0]   = g_rx[8];  g_cc.ip[1]   = g_rx[7];  g_cc.ip[2]   = g_rx[6];  g_cc.ip[3]   = g_rx[5];
        g_cc.mask[0] = g_rx[12]; g_cc.mask[1] = g_rx[11]; g_cc.mask[2] = g_rx[10]; g_cc.mask[3] = g_rx[9];
        g_cc.gw[0]   = g_rx[16]; g_cc.gw[1]   = g_rx[15]; g_cc.gw[2]   = g_rx[14]; g_cc.gw[3]   = g_rx[13];
        if (g_cc.ip[0] != 0u) g_cc.state = CC_STATE_DHCP_OK;
    } else if (op == EV_UNSOL_PING_REPORT && g_rx_len >= 13u) {
        if (get_u32le(g_rx + 9) > 0u) {
            g_cc.ping_ok = 1u;
            g_cc.ping_fail_cnt = 0u;
        }
    } else if (op == EV_UNSOL_BSD_CLOSE_WAIT && g_rx_len >= 9u) {
        int32_t sd = (int32_t)get_u32le(g_rx + 5);
        if (sd >= 0 && sd == g_cc.listen_sd) {
            g_cc.listen_sd = -1;
        }
    } else if (op == EV_UNSOL_FREE_BUFF && g_rx_len >= 9u) {
        /* Flow control: [04 00 41 arglen num_entries(u16) ... (handle u16, freed u16)] */
        uint16_t entries = get_u16le(g_rx + 5);
        for (uint16_t i = 0; i < entries && (7u + i * 4u + 4u) <= g_rx_len; i++) {
            g_cc.free_bufs += (uint8_t)get_u16le(g_rx + 7u + i * 4u + 2u);
        }
        if (g_cc.total_bufs && g_cc.free_bufs > g_cc.total_bufs)
            g_cc.free_bufs = g_cc.total_bufs;
    }
}

void cc3000_poll(void) {
    if (g_cc.state == CC_STATE_OFF) return;
    for (int i = 0; i < 16 && CC_IRQ() == 0u; i++) {
        if (cc_spi_read() > 0) handle_async_event();
        else break;
    }
}

/* Send an HCI command and wait for its matching HCI event opcode. */
static int cc_cmd(uint16_t opcode, const uint8_t *args, uint8_t arg_len, uint32_t timeout_ms) {
    cc3000_poll();
    uint8_t hci[160];
    if ((uint16_t)arg_len + 4u > sizeof(hci)) return -1;
    hci[0] = HCI_TYPE_CMND;
    put_u16le(hci + 1, opcode);
    hci[3] = arg_len;
    for (uint8_t i = 0; i < arg_len; i++) hci[4u + i] = args[i];

    int is_first = (opcode == OP_SIMPLE_LINK_START);
    if (cc_spi_write(hci, (uint16_t)(4u + arg_len), is_first) != 0) return -2;

    uint32_t t0 = g_ms;
    while ((uint32_t)(g_ms - t0) < timeout_ms) {
        if (CC_IRQ() == 0u) {
            if (cc_spi_read() > 0) {
                if (g_rx[0] == HCI_TYPE_EVNT && get_u16le(g_rx + 1) == opcode) {
                    return 0;
                }
                handle_async_event();
            }
        }
    }
    return -3;
}

void cc3000_power_off(void) {
    CC_EN_LOW();
    CC_CS_HIGH();
    g_cc.state = CC_STATE_OFF;
    g_cc.listen_sd = -1;
    for (int i = 0; i < 4; i++) g_cc.ip[i] = 0;
}

int cc3000_init(void) {
    for (uint32_t i = 0; i < sizeof(g_cc); i++) ((uint8_t *)&g_cc)[i] = 0;
    g_cc.listen_sd = -1;

    gpio_config(GPIOA, 8, GPIO_MODE_IN_FLOAT);  /* PA8 = IRQ */
    gpio_config(GPIOB, 4, GPIO_MODE_OUT_PP_50); /* PB4 = CS */
    gpio_config(GPIOB, 12, GPIO_MODE_OUT_PP_50);/* PB12 = EN */
    CC_CS_HIGH();
    CC_EN_LOW();
    spi2_init(1u, 3u); /* Mode 1 (CPOL=0, CPHA=1), 36MHz/16 = 2.25 MHz */
    delay_ms(50);

    CC_EN_HIGH();
    if (wait_irq(0u, 500u) != 0) { g_cc.state = CC_STATE_ERROR; return -1; }

    uint8_t patch = 0x00u;
    if (cc_cmd(OP_SIMPLE_LINK_START, &patch, 1, 2000u) != 0) {
        g_cc.state = CC_STATE_ERROR;
        return -2;
    }

    if (cc_cmd(OP_READ_BUFFER_SIZE, 0, 0, 1000u) == 0 && g_rx_len >= 8u) {
        g_cc.free_bufs  = g_rx[5];
        g_cc.total_bufs = g_rx[5];
        g_cc.buf_len    = get_u16le(g_rx + 6);
    } else {
        g_cc.free_bufs  = 4;
        g_cc.total_bufs = 4;
        g_cc.buf_len    = 256;
    }

    if (cc_cmd(OP_READ_SP_VERSION, 0, 0, 1000u) == 0 && g_rx_len >= 7u && g_rx[3] >= 3u) {
        g_cc.sp_ver[0] = g_rx[5];
        g_cc.sp_ver[1] = g_rx[6];
    }

    /* Enable connect/disconnect/DHCP events (mask out keepalive/ping noise) */
    uint8_t mask[4];
    put_u32le(mask, 0x00000200u); /* mask KEEPALIVE only */
    cc_cmd(OP_WLAN_SET_EVENT_MASK, mask, 4, 1000u);

    /* Disable auto-connect / fast_connect (conflicts with manual OP_WLAN_CONNECT DHCP) */
    uint8_t pol[12];
    for (int i = 0; i < 12; i++) pol[i] = 0;
    cc_cmd(OP_WLAN_SET_CONN_POLICY, pol, 12, 1000u);

    /* Enable DHCP */
    uint8_t dhcp[16];
    for (int i = 0; i < 16; i++) dhcp[i] = 0;
    cc_cmd(OP_NETAPP_DHCP, dhcp, 16, 1000u);

    /* Set timeouts: DHCP=14400s (4h default), ARP=3600s, Keepalive=10s, Inactivity=0 (never close listen socket) */
    uint8_t tmo[16];
    put_u32le(tmo + 0,  14400u);
    put_u32le(tmo + 4,  3600u);
    put_u32le(tmo + 8,  10u);
    put_u32le(tmo + 12, 0u);
    cc_cmd(OP_NETAPP_TIMEOUT_VALUES, tmo, 16, 1000u);

    cc3000_get_ipconfig();
    g_cc.state = CC_STATE_INIT;
    return 0;
}

int cc3000_get_ipconfig(void) {
    if (cc_cmd(OP_NETAPP_IPCONFIG, 0, 0, 1000u) != 0 || g_rx_len < 31u) return -1;
    /* Reply: [04 05 20 len status(1B) ip(4B) mask(4B) gw(4B) dhcp(4B) dns(4B) mac(6B) ssid(32B)] */
    g_cc.ip[0]   = g_rx[8];  g_cc.ip[1]   = g_rx[7];  g_cc.ip[2]   = g_rx[6];  g_cc.ip[3]   = g_rx[5];
    g_cc.mask[0] = g_rx[12]; g_cc.mask[1] = g_rx[11]; g_cc.mask[2] = g_rx[10]; g_cc.mask[3] = g_rx[9];
    g_cc.gw[0]   = g_rx[16]; g_cc.gw[1]   = g_rx[15]; g_cc.gw[2]   = g_rx[14]; g_cc.gw[3]   = g_rx[13];
    for (int i = 0; i < 6; i++) g_cc.mac[i] = g_rx[30 - i];
    return 0;
}

static uint8_t slen(const char *s, uint8_t maxn) {
    uint8_t n = 0;
    while (s && s[n] && n < maxn) n++;
    return n;
}

int cc3000_connect(const char *ssid, const char *pass, uint8_t sec) {
    uint8_t ssid_l = slen(ssid, 32);
    uint8_t key_l  = (sec == CC_SEC_OPEN) ? 0u : slen(pass, 64);
    if (ssid_l == 0u) return -1;

    uint8_t args[128];
    for (uint32_t i = 0; i < sizeof(args); i++) args[i] = 0;
    put_u32le(args + 0,  0x0000001Cu);
    put_u32le(args + 4,  ssid_l);
    put_u32le(args + 8,  sec);
    put_u32le(args + 12, 0x00000010u + ssid_l);
    put_u32le(args + 16, key_l);
    /* bytes 20..27 = 2B pad + 6B BSSID (zeros) */
    for (uint8_t i = 0; i < ssid_l; i++) args[28u + i] = (uint8_t)ssid[i];
    for (uint8_t i = 0; i < key_l;  i++) args[28u + ssid_l + i] = (uint8_t)pass[i];

    g_cc.state = CC_STATE_CONNECTING;
    return cc_cmd(OP_WLAN_CONNECT, args, (uint8_t)(28u + ssid_l + key_l), 3000u);
}

int32_t cc3000_tcp_listen(uint16_t port) {
    uint8_t args[28];
    put_u32le(args + 0, 2u); /* AF_INET */
    put_u32le(args + 4, 1u); /* SOCK_STREAM */
    put_u32le(args + 8, 6u); /* IPPROTO_TCP */
    if (cc_cmd(OP_SOCKET, args, 12, 1000u) != 0 || g_rx_len < 9u) return -1;
    int32_t sd = (int32_t)get_u32le(g_rx + 5);
    if (sd < 0) return -2;

    /* BIND: [sd(u32), offset=8(u32), len=16(u32), family=2(u16le), port(u16be), ip=0(u32), zero(8B)] */
    for (int i = 0; i < 28; i++) args[i] = 0;
    put_u32le(args + 0, (uint32_t)sd);
    put_u32le(args + 4, 8u);
    put_u32le(args + 8, 16u);
    put_u16le(args + 12, 2u);
    args[14] = (uint8_t)(port >> 8);
    args[15] = (uint8_t)(port & 0xFFu);
    if (cc_cmd(OP_BIND, args, 28, 1000u) != 0 || (int32_t)get_u32le(g_rx + 5) < 0) {
        cc3000_tcp_close(sd);
        return -3;
    }

    /* LISTEN: [sd(u32), backlog=1(u32)] */
    put_u32le(args + 0, (uint32_t)sd);
    put_u32le(args + 4, 1u);
    if (cc_cmd(OP_LISTEN, args, 8, 1000u) != 0 || (int32_t)get_u32le(g_rx + 5) < 0) {
        cc3000_tcp_close(sd);
        return -4;
    }

    /* SETSOCKOPT(sd, SOL_SOCKET=0xFFFF, SOCKOPT_ACCEPT_NONBLOCK=2, SOCK_ON=0, len=4) */
    put_u32le(args + 0, (uint32_t)sd);
    put_u32le(args + 4, 0xFFFFu);
    put_u32le(args + 8, 2u);
    put_u32le(args + 12, 8u); /* offset to optval */
    put_u32le(args + 16, 4u); /* optlen */
    put_u32le(args + 20, 0u); /* SOCK_ON = 0 on TI CC3000! */
    cc_cmd(OP_SETSOCKOPT, args, 24, 1000u);

    g_cc.listen_sd = sd;
    return sd;
}

static int cc_select_readable(int32_t sd, uint32_t timeout_us) {
    /* SELECT: [nfds, 0x14, 0x14, 0x14, 0x14, blocking=0 (use timeout!), rset, wset, eset, tv_sec, tv_usec] */
    uint8_t args[44];
    put_u32le(args + 0,  (uint32_t)(sd + 1));
    put_u32le(args + 4,  0x14u);
    put_u32le(args + 8,  0x14u);
    put_u32le(args + 12, 0x14u);
    put_u32le(args + 16, 0x14u);
    put_u32le(args + 20, 0u); /* 0 = use tv_sec/tv_usec timeout */
    put_u32le(args + 24, 1u << (uint32_t)sd);
    put_u32le(args + 28, 0u);
    put_u32le(args + 32, 0u);
    put_u32le(args + 36, timeout_us / 1000000u);
    put_u32le(args + 40, timeout_us % 1000000u);
    if (cc_cmd(OP_SELECT, args, 44, (timeout_us / 1000u) + 500u) != 0 || g_rx_len < 13u)
        return 0;
    int32_t status = (int32_t)get_u32le(g_rx + 5);
    uint32_t rset  = get_u32le(g_rx + 9);
    return (status > 0 && (rset & (1u << (uint32_t)sd))) ? 1 : 0;
}

int cc3000_wlan_status(void) {
    if (cc_cmd(OP_WLAN_IOCTL_STATUSGET, 0, 0, 1000u) != 0 || g_rx_len < 9u)
        return -1;
    return (int)get_u32le(g_rx + 5);
}

int cc3000_ping_gw(void) {
    if (g_cc.state != CC_STATE_DHCP_OK || g_cc.gw[0] == 0u) return -1;
    uint8_t args[16];
    args[0] = g_cc.gw[0];
    args[1] = g_cc.gw[1];
    args[2] = g_cc.gw[2];
    args[3] = g_cc.gw[3];
    put_u32le(args + 4,  1u);    /* 1 attempt */
    put_u32le(args + 8,  32u);   /* 32-byte payload */
    put_u32le(args + 12, 1000u); /* 1000 ms timeout */
    g_cc.ping_ok = 0u;
    return cc_cmd(OP_NETAPP_PING_SEND, args, 16, 1000u);
}

int32_t cc3000_tcp_accept_nb(int32_t listen_sd) {
    static uint8_t err_cnt = 0;
    if (listen_sd < 0 || g_cc.state != CC_STATE_DHCP_OK) return -1;
    uint8_t args[4];
    put_u32le(args, (uint32_t)listen_sd);
    if (cc_cmd(OP_ACCEPT, args, 4, 1000u) != 0 || g_rx_len < 13u) {
        if (++err_cnt >= 5u) {
            err_cnt = 0;
            g_cc.state = CC_STATE_ERROR;
            g_cc.listen_sd = -1;
        }
        return -1;
    }
    err_cnt = 0;
    int32_t csd = (int32_t)get_u32le(g_rx + 9);
    if (csd >= 0) return csd;
    if (csd != -2) {
        /* Anything other than SOC_IN_PROGRESS (-2) means listen_sd died */
        cc3000_tcp_close(listen_sd);
        g_cc.listen_sd = -1;
    }
    return -1;
}

int32_t cc3000_tcp_recv(int32_t sd, uint8_t *buf, uint16_t max_len, uint32_t timeout_ms) {
    if (!cc_select_readable(sd, timeout_ms * 1000u)) return 0;
    if (max_len > 200u) max_len = 200u;
    uint8_t args[12];
    put_u32le(args + 0, (uint32_t)sd);
    put_u32le(args + 4, max_len);
    put_u32le(args + 8, 0u);
    if (cc_cmd(OP_RECV, args, 12, 1000u) != 0 || g_rx_len < 13u) return -1;
    int32_t nbytes = (int32_t)get_u32le(g_rx + 9);
    if (nbytes <= 0) return nbytes;

    /* Next packet from CC3000 is HCI_TYPE_DATA (0x02), OP_DATA_RECV (0x85) */
    uint32_t t0 = g_ms;
    while ((uint32_t)(g_ms - t0) < 1000u) {
        if (CC_IRQ() == 0u && cc_spi_read() > 0) {
            if (g_rx[0] == HCI_TYPE_DATA && g_rx[1] == OP_DATA_RECV) {
                uint8_t arg_l = g_rx[2];
                uint16_t data_off = (uint16_t)(5u + arg_l);
                int32_t avail = (int32_t)g_rx_len - (int32_t)data_off;
                if (avail > nbytes) avail = nbytes;
                if (avail > (int32_t)max_len) avail = max_len;
                for (int32_t i = 0; i < avail; i++) buf[i] = g_rx[data_off + i];
                return avail;
            }
            handle_async_event();
        }
    }
    return -1;
}

int32_t cc3000_tcp_send(int32_t sd, const uint8_t *buf, uint16_t len) {
    uint16_t sent = 0;
    uint16_t max_chunk = (g_cc.buf_len > 64u) ? (uint16_t)(g_cc.buf_len - 48u) : 128u;
    if (max_chunk > 480u) max_chunk = 480u;

    while (sent < len) {
        cc3000_poll();
        uint32_t t0 = g_ms;
        while (g_cc.free_bufs == 0u) {
            cc3000_poll();
            if ((uint32_t)(g_ms - t0) > 2000u) return -1;
        }
        uint16_t chunk = (uint16_t)(len - sent);
        if (chunk > max_chunk) chunk = max_chunk;

        uint8_t pkt[512];
        pkt[0] = HCI_TYPE_DATA;
        pkt[1] = OP_DATA_SEND;
        pkt[2] = 16u; /* args length */
        put_u16le(pkt + 3, (uint16_t)(16u + chunk));
        put_u32le(pkt + 5, (uint32_t)sd);
        put_u32le(pkt + 9, 12u); /* data offset from args[4] */
        put_u32le(pkt + 13, chunk);
        put_u32le(pkt + 17, 0u); /* flags */
        for (uint16_t i = 0; i < chunk; i++) pkt[21u + i] = buf[sent + i];

        g_cc.free_bufs--;
        if (cc_spi_write(pkt, (uint16_t)(21u + chunk), 0) != 0) return -2;
        sent += chunk;
        if (sent < len) {
            /* Drain EV_UNSOL_FREE_BUFF before next chunk to avoid SPI collision */
            uint32_t tw = g_ms;
            while (g_cc.free_bufs < g_cc.total_bufs && (uint32_t)(g_ms - tw) < 150u) {
                cc3000_poll();
            }
        }
    }
    return (int32_t)sent;
}

void cc3000_tcp_close(int32_t sd) {
    if (sd < 0) return;
    /* Let CC3000 flush all in-flight TX buffers and deliver the TCP segment
       before closing, otherwise CC3000 aborts the socket with a TCP RST. */
    uint32_t t0 = g_ms;
    while (g_cc.free_bufs < g_cc.total_bufs && (uint32_t)(g_ms - t0) < 250u) {
        cc3000_poll();
    }
    uint32_t t1 = g_ms;
    while ((uint32_t)(g_ms - t1) < 60u) {
        cc3000_poll();
    }
    uint8_t args[4];
    put_u32le(args, (uint32_t)sd);
    cc_cmd(OP_CLOSE_SOCKET, args, 4, 1000u);
}
