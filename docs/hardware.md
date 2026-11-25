# Displio V1.0 — Hardware Map

Extracted 100% over USB via the Phase 2 hardware probe (`probe/probe.c`) and disassembly of the 16 KiB bootloader (`0x08000000`–`0x08003FFF`).

## Microcontroller & Memory

| Item | Value |
|---|---|
| Core | ARM Cortex-M3 r1p1 (`CPUID = 0x411FC231`) |
| Family / Part | **STM32F103xB** (Medium-Density, 128 KiB Flash, 20 KiB SRAM) |
| Flash | `0x08000000`–`0x0801FFFF` (128 KiB, 1 KiB pages) |
| SRAM | `0x20000000`–`0x20004FFF` (20 KiB) |
| Option Bytes | `RDP = 0xA5` (unprotected), no write protection |
| Clock Tree | 8 MHz HSE crystal × PLL×9 = **72 MHz SYSCLK**, APB2 = 72 MHz, APB1 = 36 MHz, USB = 48 MHz (`RCC_CFGR = 0x001D040A`) |
| Debug | SWD enabled (`AFIO_MAPR = 0x02000000`, JTAG disabled) |

## Bootloader & Recovery Mechanism

- **Status word in Flash**: `*(uint32_t *)0x08003C00` (inside bootloader page 15; returned in `GET_INFO` bytes `5..8`):
  - `0xFFFFFFFF`: application erased
  - `0x0000FFFF`: written, not yet validated
  - `0x00000000`: validated via `0x06` (bootloader jumps to `0x08004000` at reset)
- **Software entry into bootloader** (from custom firmware, no button needed):
  1. Enable `PWR` and `BKP` clocks (`RCC_APB1ENR |= (1<<28) | (1<<27)`).
  2. Enable backup domain write (`PWR_CR |= (1<<8)`).
  3. Write any non-zero value to **`BKP_DR1`** (`0x40006C04`).
  4. Call `NVIC_SystemReset()`.
  5. The bootloader reads `BKP_DR1`, clears it to `0`, and stays in USB HID bootloader mode.
- **Hardware unbrick button**:
  - Hold the single physical button (**`PA2`**) while plugging in USB (**`PA9`** = VBUS) for **1.45 seconds** (`1450 ms`).
  - The bootloader beeps twice, flashes its splash screen on the E-Ink display, and stays in USB HID bootloader mode regardless of the application flash contents.

## Confirmed Pinout

| Pin | Direction / AF | Function | Notes |
|---|---|---|---|
| `PA0` | Analog (`ADC1_IN0`) | **VBAT_ADC** | Battery voltage divider output (~2.31 V when charging; gated by `PC13`) |
| `PA1` | Output PP | Power / System latch | Set LOW at boot, HIGH after 50 ms, LOW on USB reset |
| `PA2` | Input floating | **User Button** | Active HIGH (`1` = pressed) |
| `PA3` | Output PP | **EPD_BORDER** | Pervasive Displays 2.7" G2 border control |
| `PA4` | Output PP | **EPD_DISCHARGE** | E-Ink panel discharge |
| `PA5` | AF PP (`SPI1_SCK`) | **EPD_SCK** | SPI1 clock |
| `PA6` | Input floating (`SPI1_MISO`) | **EPD_MISO** | SPI1 MISO |
| `PA7` | AF PP (`SPI1_MOSI`) | **EPD_MOSI** | SPI1 MOSI |
| `PA8` | Input floating | **WIFI_IRQ** | TI CC3000 `SPI_IRQ` (active LOW) |
| `PA9` | Input floating | **USB VBUS Sense** | `1` = USB cable plugged |
| `PA10` | Output PP | **USB D+ Pull-Up** | Active LOW (`0` = USB connected, `1` = disconnected) |
| `PA11` / `PA12` | USB | **USB D- / D+** | STM32 USB Full-Speed device |
| `PB0` | AF PP (`TIM3_CH3`) | **Piezo Buzzer** | PWM ~2730 Hz (`PSC=9, ARR=2637, CCR3=1318`); LOW PP when off |
| `PB1` | Output PP | **EPD_PANEL_ON** | Active LOW (P-MOSFET power switch for E-Ink panel) |
| `PB2` | Output PP | **EPD_RESET** | Active LOW reset for E-Ink COG |
| `PB4` | Output PP | **WIFI_CS** | TI CC3000 SPI chip select (active LOW) |
| `PB6` / `PB7` | I2C1 (`SCL`/`SDA`) | **I2C1 Bus** | External pull-ups; hosts `0x1C` (`MMA8452Q`) and `0x40` (`Si7021`) |
| `PB10` | Input floating | **EPD_BUSY** | `1` = E-Ink controller busy |
| `PB11` | Output PP | **EPD_CS** | Active LOW SPI chip select for E-Ink |
| `PB12` | Output PP | **WIFI_EN** | TI CC3000 `VBAT_SW_EN` (active HIGH; held LOW by bootloader) |
| `PB13` / `PB14` / `PB15` | `SPI2` (`SCK`/`MISO`/`MOSI`) | **WIFI_SPI** | TI CC3000 SPI bus (SPI Mode 1: `CPOL=0, CPHA=1`) |
| `PC13` | Output PP | **BAT_MEAS_EN** | Active HIGH: enables resistor divider feeding `PA0` (`ADC1_IN0`) |

## Confirmed Peripherals (100% Verified Live)

- **Accelerometer (`I2C1 0x1C`)**: **NXP/Freescale `MMA8452Q`** (`WHO_AM_I` reg `0x0D` = `0x2A`) — 3-axis acceleration (mg) + portrait/landscape orientation detection.
- **Temperature & Humidity (`I2C1 0x40`)**: **Silicon Labs `Si7021`** (User Reg `0xE7` = `0x3A`, FW rev `0x84B8` = `0x20` = v2.0) — high-accuracy ambient temperature (°C) and relative humidity (%).
- **Wi-Fi Module (`SPI2`)**: **Texas Instruments `CC3000`** (`EN=PB12`, `CS=PB4`, `IRQ=PA8`, `SPI2` Mode 1) — verified `HCI_CMND_SIMPLE_LINK_START` (`0x4000`) → `HCI_EVNT_SIMPLE_LINK_START` (`04 00 40 01 00`).
- **Battery / Power**: **`VDDA ≈ 2.96 V`** (via internal `Vrefint`), **`VBAT`** via `PC13` enable → `PA0` (`ADC1_IN0`, ~2.31 V divided on USB charge).
- **E-Ink Display (`SPI1`)**: **Pervasive Displays 2.7" monochrome E-Ink** (`264 × 176`, 5808 B 1-bpp framebuffer, G2 COG driver) — verified crisp & readable.
- **Audio**: **Piezo Buzzer** on `PB0` (`TIM3_CH3`).
