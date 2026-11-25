# Product Requirements Document (PRD) — Displio Rebirth

## 1. Vision & Purpose

The **Displio V1.0** (Draugiem Group, 2015 Kickstarter) is a well-built 2.7" E-Ink Wi-Fi desk display equipped with temperature/humidity sensors, an accelerometer, a piezo buzzer, and a rechargeable battery. Its original cloud backend has been discontinued, rendering surviving devices unusable.

**Displio Rebirth** is an independent, **non-profit**, volunteer-driven open-source project that replaces the proprietary cloud-dependent firmware with a clean, bare-metal, local-first firmware — enabling owners to reuse their hardware with **Home Assistant** or any local automation script, with zero cloud dependency and zero hardware modification.

> [!NOTE]
> This project is not affiliated with, endorsed by, or associated with Displio or Draugiem Group in any way.

---

## 2. Architecture Overview

The ecosystem is intentionally split into **two independent repositories** connected by a clean, versioned local HTTP REST API ([docs/api.md](api.md)):

```
┌─────────────────────────────────────────┐       Local HTTP/1.1 (Wi-Fi)       ┌──────────────────────────────────────┐
│  Ldo-fr/displio-rebirth  (this repo)    │ ◄────────────────────────────────► │  Ldo-fr/displio-rebirth-ha (HACS)    │
│                                         │   GET  /status   (JSON telemetry)  │                                      │
│  • STM32F103xB bare-metal C firmware    │   POST /display  (5808B 1-bpp)     │  • Home Assistant Custom Integration │
│  • Pervasive Displays 2.7" E-Ink driver │   POST /beep     (piezo buzzer)    │  • Config Flow (IP/port validation)  │
│  • Si7021, MMA8452Q, Battery ADC        │                                    │  • Sensors & Binary Sensors          │
│  • TI CC3000 Wi-Fi + HTTP REST server   │                                    │  • Pillow 264×176 text/image render  │
│  • USB HID flasher & provisioning CLI   │                                    │  • HA Services (display_text, beep)  │
└─────────────────────────────────────────┘                                    └──────────────────────────────────────┘
```

### Why Two Repositories?
- **Separation of concerns**: Embedded C firmware (`arm-none-eabi-gcc`) and Python Home Assistant HACS integration follow completely different release cycles, CI checks (`hassfest` / `hacs/action`), and contributor skill sets.
- **Standalone usability**: Users who don't use Home Assistant can still drive the Displio from `curl`, Python, Node-RED, or cron jobs using only `displio-rebirth`.

---

## 3. Hardware Constraints & Target Specifications

All hardware details were empirically verified on a sealed Displio V1.0 unit (see [docs/hardware.md](hardware.md)):

| Subsystem | Component / Spec | Key Constraints |
|---|---|---|
| **MCU** | STM32F103xB (Cortex-M3, 72 MHz) | 128 KiB Flash (`112 KiB` app), **20 KiB SRAM** (11.6 KiB used by double E-Ink framebuffer) |
| **Bootloader** | Factory USB HID (`0483:a197`) | Protected `0x08000000`–`0x08003FFF`; validates STM32 CRC-32 at `0x0801FFFC`; unbrickable via `PA2` hold on USB plug |
| **Display** | Pervasive Displays 2.7" G2 COG E-Ink | `264 × 176` pixels, 1-bpp monochrome (`5,808` bytes), SPI1 |
| **Wi-Fi** | Texas Instruments `CC3000` | 2.4 GHz only, SPI2 Mode 1, limited internal socket buffers, no modern TLS 1.2/1.3 |
| **Sensors** | `Si7021` (`0x40`), `MMA8452Q` (`0x1C`) | Shared `I2C1` bus (`PB6`/`PB7`) |
| **Power** | Battery ADC (`PC13` → `PA0`), `VBUS` (`PA9`) | Gated voltage divider; `VDDA` measured via `Vrefint` |
| **Audio / UI** | Piezo buzzer (`PB0` `TIM3_CH3`), button (`PA2`) | Active-HIGH button, PWM buzzer (~2.4–2.7 kHz resonance) |

---

## 4. Functional Requirements

| ID | Requirement | Status |
|---|---|---|
| **FR-1** | Flash custom firmware 100% over USB without opening the enclosure or soldering. | Done |
| **FR-2** | Hands-free software reboot into USB bootloader (`BKP_DR1 = 0xB007`) + hardware button recovery (`PA2`). | Done |
| **FR-3** | Render crisp 264×176 1-bpp images on the 2.7" E-Ink panel (full and fast refresh). | Done |
| **FR-4** | Read live ambient temperature, humidity, 3-axis acceleration/orientation, battery ADC, USB power, and button state. | Done |
| **FR-5** | Drive the onboard piezo buzzer with configurable frequency and duration. | Done |
| **FR-6** | Provision Wi-Fi credentials over USB and persist them across reboots in Flash (`0x0801F800`) without breaking the bootloader CRC page (`0x0801FC00`). | Done |
| **FR-7** | Auto-connect to Wi-Fi on boot, auto-reconnect on drop, and expose a stable local HTTP/1.1 REST API (`GET /status`, `POST /display`, `POST /beep`, `GET /`). | Done |
| **FR-8** | Integrate cleanly with Home Assistant via HACS (`displio-rebirth-ha`). | Planned (Phase 7) |

---

## 5. Non-Functional Requirements

- **Zero brick risk**: Never erase or modify the factory bootloader (`0x08000000`–`0x08003FFF`).
- **Tiny footprint**: Bare-metal `-nostdlib` C firmware fitting comfortably under 20 KiB Flash and 14 KiB SRAM (currently ~12.7 KiB Flash, ~12.5 KiB RAM).
- **Zero host dependencies**: Host CLI tools (`tools/displio_tool.py`, `tools/displio_app.py`) use only the Python 3 standard library.
- **Local-first & privacy**: No telemetry, no cloud endpoints, no hardcoded credentials. Wi-Fi credentials stay strictly in device Flash.
- **Security model**: Plain HTTP over LAN only (documented in [SECURITY.md](../SECURITY.md)), intended for trusted home networks / IoT VLANs.

---

## 6. Milestones & Roadmap

| Phase | Scope | Status |
|---|---|---|
| **Phase 1** | Reverse-engineer USB HID bootloader protocol (`docs/protocol.md`) & build `tools/displio_tool.py` | Completed |
| **Phase 2** | Hardware discovery probe, full MCU/pinout map (`docs/hardware.md`), CRC-32 `flash --boot` | Completed |
| **Phase 3** | Bare-metal custom firmware: USB HID, 2.7" E-Ink driver, piezo buzzer, `BKP_DR1` bootloader re-entry | Completed |
| **Phase 4** | Peripheral drivers: `MMA8452Q`, `Si7021`, gated battery ADC, TI `CC3000` Wi-Fi bring-up | Completed |
| **Phase 5** | TI `CC3000` Wi-Fi STA driver, persistent Flash config, HTTP/1.1 REST API (`docs/api.md`) | Completed |
| **Phase 6** | Public open-source launch: human-friendly README, PRD, MIT license, CI builds & Releases | Completed |
| **Phase 7** | Companion Home Assistant HACS integration (`Ldo-fr/displio-rebirth-ha`) | Next |
| **Future** | Battery voltage divider calibration (`vbat_mv` / `%`), optional deep-sleep battery mode, button webhook/push events | Backlog |
