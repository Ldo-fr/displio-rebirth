# displio-rebirth

Give a second life to the **Displio V1.0** e-ink desk display (Draugiem Group, 2015)
whose cloud service is gone: replace its firmware with an open one, updatable over USB.

Original from : https://www.kickstarter.com/projects/914874115/displio-wifi-display-that-tracks-whats-important-t

## Status

- [x] USB HID bootloader protocol reverse-engineered — see [docs/protocol.md](docs/protocol.md)
- [x] Host tool: info / read / dump / erase / write + verify — `tools/displio_tool.py`
- [ ] Understand boot hand-off (`0x06`) and the way back into the bootloader
- [ ] Hardware discovery payload (MCU id, bootloader dump, pins)
- [ ] Custom firmware: USB control of e-ink display, accelerometer, buzzer
- [ ] Wi-Fi mode + local server

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
