# Displio Rebirth — HTTP REST API (`v1.0`)

This document defines the stable local HTTP API exposed by the Displio custom firmware over its **TI CC3000** Wi-Fi interface (default TCP port `80`). It serves as the integration contract for the companion Home Assistant integration (`displio-rebirth-ha`) as well as any custom scripts.

---

## Provisioning Wi-Fi (over USB HID)

Credentials are stored persistently in STM32 Flash (`0x0801F800`, 1 KiB page) and automatically used at every boot:

```sh
# DHCP mode
python3 tools/displio_app.py wifi-setup "<SSID>" "<PASSWORD>" --sec wpa2 --port 80

# Static IP mode (recommended — skips CC3000 DHCP wait; --gw defaults to x.y.z.1)
python3 tools/displio_app.py wifi-setup "<SSID>" "<PASSWORD>" --ip 192.168.1.42 --mask 255.255.255.0 --gw 192.168.1.1

python3 tools/displio_app.py wifi-status
```

---

## Endpoints

### `GET /status`
Returns a live JSON snapshot of all onboard sensors, power rails, and network metadata.

**Response (`200 OK`, `application/json`)**:
```json
{
  "device": "displio-rebirth",
  "fw": "1.0",
  "uptime_ms": 123456,
  "temp_c": 24.50,
  "humidity": 45.2,
  "vdda_mv": 2960,
  "vbat_adc_mv": 2311,
  "accel_mg": [991, 20, -5],
  "orientation": 2,
  "button": 0,
  "vbus": 1,
  "ip": "192.168.1.42",
  "mac": "08:00:28:5a:bc:de"
}
```

| Field | Source | Unit / Values |
|---|---|---|
| `temp_c` | `Si7021` (`I2C1 0x40`) | Ambient temperature in °C |
| `humidity` | `Si7021` (`I2C1 0x40`) | Relative humidity in % |
| `vdda_mv` | Internal `Vrefint` | Analog supply in mV (~2960 mV) |
| `vbat_adc_mv` | `PC13` → `PA0` (`ADC1_IN0`) | Divided battery voltage in mV |
| `accel_mg` | `MMA8452Q` (`I2C1 0x1C`) | `[X, Y, Z]` acceleration in mg |
| `orientation` | `MMA8452Q` `PL_STATUS` | `0`=Portrait Up, `1`=Portrait Down, `2`=Landscape Right, `3`=Landscape Left |
| `button` | `PA2` | `1` = pressed, `0` = released |
| `vbus` | `PA9` | `1` = USB plugged, `0` = battery |

---

### `POST /display[?full=1]`
Uploads a raw **5,808-byte** 1-bpp monochrome framebuffer (`264 × 176` pixels, row-major, MSB-first, `1` = black, `0` = white) and refreshes the 2.7" E-Ink panel.

- Query parameter `full=1` (optional): performs a full black/white flash clear before drawing.
- **Request**: `Content-Length: 5808`, raw binary body (`application/octet-stream`).
- **Response (`200 OK`)**: `{"ok":true}` (returned immediately before the ~3 s E-Ink refresh begins).

---

### `POST /beep[?freq=2400&ms=80]`
Triggers the onboard piezo buzzer (`PB0` / `TIM3_CH3`).

- `freq` (optional): frequency in Hz (`100`–`10000`, default `2400`).
- `ms` (optional): duration in ms (`1`–`2000`, default `80`).
- **Response (`200 OK`)**: `{"ok":true}`.

---

### `GET /`
Simple plain-text healthcheck (`200 OK`).
