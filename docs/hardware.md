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

## Confirmed Pinout (from Bootloader)

| Pin | Direction / AF | Function | Notes |
|---|---|---|---|
| `PA1` | Output PP | Power / System latch | Set LOW at boot, HIGH after 50 ms, LOW on USB reset |
| `PA2` | Input floating | **User Button** | Active HIGH (`1` = pressed) |
| `PA3` | Output PP | **EPD_BORDER** | Pervasive Displays 2.7" G2 border control |
| `PA4` | Output PP | **EPD_DISCHARGE** | E-Ink panel discharge |
| `PA5` | AF PP (`SPI1_SCK`) | **EPD_SCK** | SPI1 clock |
| `PA6` | Input floating (`SPI1_MISO`) | **EPD_MISO** | SPI1 MISO |
| `PA7` | AF PP (`SPI1_MOSI`) | **EPD_MOSI** | SPI1 MOSI |
| `PA9` | Input floating | **USB VBUS Sense** | `1` = USB cable plugged |
| `PA10` | Output PP | **USB D+ Pull-Up** | Active LOW (`0` = USB connected, `1` = disconnected) |
| `PA11` / `PA12` | USB | **USB D- / D+** | STM32 USB Full-Speed device |
| `PB0` | AF PP (`TIM3_CH3`) | **Piezo Buzzer** | PWM ~2730 Hz (`PSC=9, ARR=2637, CCR3=1318`); LOW PP when off |
| `PB1` | Output PP | **EPD_PANEL_ON** | Active LOW (P-MOSFET power switch for E-Ink panel) |
| `PB2` | Output PP | **EPD_RESET** | Active LOW reset for E-Ink COG |
| `PB10` | Input floating | **EPD_BUSY** | `1` = E-Ink controller busy |
| `PB11` | Output PP | **EPD_CS** | Active LOW SPI chip select for E-Ink |
| `PB12` | Output PP | Unknown (reset LOW at boot) | Candidate: SPI2 CS / Wi-Fi / Flash |
| `PC13` | Output PP | Unknown (configured PP at boot) | Candidate: LED / Wi-Fi / Power |

## E-Ink Display

- **Panel**: Pervasive Displays **2.7" monochrome E-Ink** (`264 × 176` pixels = `5808` bytes 1-bpp framebuffer) with **G2 COG driver** (`0x70` register index, `0x72` data write, `0x73` data read, `0x71` COG ID read).
- **Bootloader driver**: Complete working power-on (`0x080022F0`), 4-stage update (`0x08002180`), and power-off (`0x08001E88`) implementation verified in the bootloader dump.

## Still to Discover (Phase 3 Bus Scanner)

- **Accelerometer** (likely I2C1 `PB6/PB7` or I2C2 `PB10/PB11` — note `PB10/PB11` are used by EPD, so likely **I2C1 `PB6/PB7`** or **SPI2 `PB13/PB14/PB15`**).
- **Wi-Fi module** (likely USART1/2/3 or SPI2 — exact pins and chip model to be probed from our Phase 3 USB firmware).
- **External SPI Flash** (if present) and **Battery ADC** channel.
