#ifndef CC3000_H
#define CC3000_H

#include <stdint.h>

#define CC_SEC_OPEN 0u
#define CC_SEC_WEP  1u
#define CC_SEC_WPA  2u
#define CC_SEC_WPA2 3u

typedef enum {
    CC_STATE_OFF = 0,
    CC_STATE_INIT,
    CC_STATE_CONNECTING,
    CC_STATE_CONNECTED,
    CC_STATE_DHCP_OK,
    CC_STATE_ERROR,
} cc_state_t;

typedef struct {
    cc_state_t state;
    uint8_t    sp_ver[2];
    uint8_t    ip[4];     /* MSB first: ip[0].ip[1].ip[2].ip[3] */
    uint8_t    mask[4];
    uint8_t    gw[4];
    uint8_t    mac[6];
    uint8_t    free_bufs;
    uint8_t    total_bufs;
    uint16_t   buf_len;
    int32_t    listen_sd;
    uint8_t    ping_ok;
    uint8_t    ping_fail_cnt;
    uint8_t    reconn_cnt;
    int8_t     last_wlan_st;
} cc3000_t;

extern cc3000_t g_cc;

int     cc3000_init(void);
void    cc3000_power_off(void);
int     cc3000_connect(const char *ssid, const char *pass, uint8_t sec);
void    cc3000_poll(void);
int     cc3000_get_ipconfig(void);
int     cc3000_wlan_status(void);
int     cc3000_ping_gw(void);

/* Minimal BSD-style TCP server sockets */
int32_t cc3000_tcp_listen(uint16_t port);
int32_t cc3000_tcp_accept_nb(int32_t listen_sd);
int32_t cc3000_tcp_recv(int32_t sd, uint8_t *buf, uint16_t max_len, uint32_t timeout_ms);
int32_t cc3000_tcp_send(int32_t sd, const uint8_t *buf, uint16_t len);
void    cc3000_tcp_close(int32_t sd);

#endif
