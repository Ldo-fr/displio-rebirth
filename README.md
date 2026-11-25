# displio-rebirth

Give a second life to the **Displio V1.0** e-ink desk display (Draugiem Group, 2015)
whose cloud service is gone: replace its firmware with an open one, updatable over USB.

Original from : https://www.kickstarter.com/projects/914874115/displio-wifi-display-that-tracks-whats-important-t

## Status

- [x] USB HID bootloader protocol reverse-engineered — see [docs/protocol.md](docs/protocol.md)
- [x] Host tool: info / read / dump / erase / write / flash (CRC-32 + validate + boot) — `tools/displio_tool.py`
- [x] Boot hand-off (`0x06` STM32 CRC-32 at `0x0801FFFC`) and recovery (`BKP_DR1` + hold button `PA2` 1.5s on USB plug)
- [x] Hardware discovery: **STM32F103xB** (72 MHz, 128 KiB Flash, 20 KiB RAM), Pervasive Displays 2.7" G2 E-Ink pinout, buzzer, button, USB pull-up — see [docs/hardware.md](docs/hardware.md)
- [x] Custom firmware (`firmware/`): USB HID (`DISPLIO REBIRTH`), Pervasive Displays 2.7" E-Ink driver, piezo buzzer, hands-free bootloader return (`BKP_DR1`)
- [x] Full peripheral discovery & drivers: `MMA8452Q` accelerometer (`I2C1 0x1C`), `Si7021` temp/humidity (`I2C1 0x40`), Battery ADC (`PC13` → `PA0`), and **TI `CC3000` Wi-Fi** (`SPI2`, `PB12=EN`, `PB4=CS`, `PA8=IRQ`)
- [ ] CC3000 Wi-Fi driver (STA connect + TCP/HTTP API) & companion Home Assistant HACS integration (`displio-rebirth-ha`)

## Building & Flashing Custom Firmware

Requires `gcc-arm-none-eabi` and Linux hidraw access (`99-displio.rules`):

```sh
# 1. Build firmware (STM32F103xB, linked at 0x08004000)
make -C firmware

# 2. Flash from bootloader mode and boot immediately
python3 tools/displio_tool.py flash build/firmware/displio-fw.bin --boot --yes

# 3. Interact with the running custom firmware over USB HID
python3 tools/displio_app.py info
python3 tools/displio_app.py beep --freq 2700 --ms 60
python3 tools/displio_app.py epd-test --full
python3 tools/displio_app.py scan

# 4. Return hands-free to the USB bootloader for the next update
python3 tools/displio_app.py bootloader
```

## Usage

Requires Linux and read/write access to the hidraw node, e.g. with a udev rule:

```
# /etc/udev/rules.d/99-displio.rules
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="a197", MODE="0666"
```

```sh
python3 tools/displio_tool.py info
python3 tools/displio_tool.py read 0x08004000 64
python3 tools/displio_tool.py dump app.bin
python3 tools/displio_tool.py erase --yes          # destructive
python3 tools/displio_tool.py write fw.bin --yes   # erase + write + verify
```

Only `0x08004000`–`0x0801FFFF` (application area) is accessible; the
bootloader at `0x08000000` is protected by the device and is never touched.
