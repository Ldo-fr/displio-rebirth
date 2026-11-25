#!/usr/bin/env python3
"""displio_app.py — host CLI for the displio-rebirth custom firmware (Phase 3)."""
from __future__ import annotations

import argparse
import os
import select
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from displio_tool import crc16_en13757, find_hidraw, wait_for_device, DisplioError

MAGIC_REQ = 0xA5
MAGIC_RESP = 0x5A

CMD_INFO = 0x10
CMD_BEEP = 0x11
CMD_FB_WRITE = 0x12
CMD_FB_CLEAR = 0x13
CMD_EPD_SHOW = 0x14
CMD_SCAN_I2C = 0x15
CMD_SCAN_IO = 0x16
CMD_SCAN_SPI2 = 0x17
CMD_BOOTLOADER = 0x7F

EPD_W, EPD_H = 264, 176
EPD_FB_SIZE = EPD_W * EPD_H // 8  # 5808 bytes


class DisplioApp:
    def __init__(self, path: str | None = None):
        self.path = path or find_hidraw()
        self.fd = os.open(self.path, os.O_RDWR | os.O_NONBLOCK)
        while select.select([self.fd], [], [], 0.01)[0]:
            os.read(self.fd, 64)

    def close(self):
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def cmd(self, opcode: int, payload: bytes = b"", timeout: float = 1.0) -> bytes:
        body = bytes([MAGIC_REQ, opcode]) + payload
        body = body.ljust(62, b"\x00")[:62]
        pkt = body + struct.pack("<H", crc16_en13757(body))
        os.write(self.fd, b"\x00" + pkt)
        if not select.select([self.fd], [], [], timeout)[0]:
            raise DisplioError(f"timeout waiting for reply to app cmd 0x{opcode:02x}")
        resp = os.read(self.fd, 64)
        if len(resp) != 64 or crc16_en13757(resp[:62]) != struct.unpack("<H", resp[62:64])[0]:
            raise DisplioError(f"bad reply CRC: {resp.hex()}")
        if resp[0] != MAGIC_RESP or resp[1] != opcode:
            raise DisplioError(f"unexpected reply header: {resp[:4].hex()}")
        if resp[2] != 0:
            raise DisplioError(f"app cmd 0x{opcode:02x} returned status 0x{resp[2]:02x} (data={resp[3:8].hex()})")
        return resp[3:62]

    def info(self) -> dict:
        d = self.cmd(CMD_INFO)
        return {
            "tag": d[:8].decode("ascii", errors="replace"),
            "uptime_ms": struct.unpack_from("<I", d, 8)[0],
            "button": d[12],
            "vbus": d[13],
            "has_prev_fb": d[14],
        }

    def beep(self, freq_hz: int = 2400, dur_ms: int = 80):
        self.cmd(CMD_BEEP, struct.pack("<HH", freq_hz, dur_ms), timeout=max(1.0, dur_ms / 1000.0 + 0.5))

    def send_fb(self, fb: bytes):
        if len(fb) != EPD_FB_SIZE:
            raise DisplioError(f"framebuffer must be {EPD_FB_SIZE} bytes (got {len(fb)})")
        for off in range(0, EPD_FB_SIZE, 56):
            chunk = fb[off:off + 56]
            self.cmd(CMD_FB_WRITE, struct.pack("<HB", off, len(chunk)) + chunk)

    def refresh(self, full_clear: bool = False):
        self.cmd(CMD_EPD_SHOW, bytes([1 if full_clear else 0]), timeout=15.0)

    def scan_i2c(self, remap: int = 0) -> list[tuple[int, int, int, int]]:
        d = self.cmd(CMD_SCAN_I2C, bytes([remap]), timeout=2.0)
        n = d[0]
        return [tuple(d[1 + i * 4:1 + (i + 1) * 4]) for i in range(n)]

    def scan_io(self) -> tuple[list[int], list[int]]:
        d = self.cmd(CMD_SCAN_IO, timeout=2.0)
        idr = list(struct.unpack_from("<3H", d, 0))
        adc = list(struct.unpack_from("<11H", d, 6))
        return idr, adc

    def scan_spi2(self) -> list[tuple[str, int, int, int]]:
        d = self.cmd(CMD_SCAN_SPI2, timeout=2.0)
        n = d[0]
        ports = "ABC"
        res = []
        for i in range(n):
            pp, b0, b1, b2 = d[1 + i * 4:1 + (i + 1) * 4]
            name = f"P{ports[pp >> 4]}{pp & 0xF}"
            res.append((name, b0, b1, b2))
        return res

    def enter_bootloader(self):
        self.cmd(CMD_BOOTLOADER, timeout=1.0)


def make_test_pattern() -> bytearray:
    """Generate a crisp 264x176 1-bpp test card with border, checkerboard, and 'DISPLIO REBIRTH' banner."""
    fb = bytearray(EPD_FB_SIZE)

    def set_px(x: int, y: int, black: bool = True):
        if 0 <= x < EPD_W and 0 <= y < EPD_H:
            idx = y * (EPD_W // 8) + (x >> 3)
            mask = 0x80 >> (x & 7)
            if black:
                fb[idx] |= mask
            else:
                fb[idx] &= ~mask

    # Outer double border
    for x in range(EPD_W):
        for y in (4, 5, 6, EPD_H - 7, EPD_H - 6, EPD_H - 5):
            set_px(x, y)
    for y in range(EPD_H):
        for x in (4, 5, 6, EPD_W - 7, EPD_W - 6, EPD_W - 5):
            set_px(x, y)

    # Top black header bar
    for y in range(14, 54):
        for x in range(14, EPD_W - 14):
            set_px(x, y)

    # Minimal 5x7 font for "DISPLIO REBIRTH"
    FONT = {
        "D": [0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E],
        "I": [0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E],
        "S": [0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E],
        "P": [0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10],
        "L": [0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F],
        "O": [0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E],
        "R": [0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11],
        "E": [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F],
        "B": [0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E],
        "T": [0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04],
        "H": [0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11],
        "V": [0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04],
        "1": [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E],
        "0": [0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E],
        ".": [0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C],
        " ": [0x00] * 7,
    }

    def draw_text(text: str, x0: int, y0: int, scale: int, black: bool):
        cx = x0
        for ch in text:
            rows = FONT.get(ch.upper(), FONT[" "])
            for ry, bits in enumerate(rows):
                for rx in range(5):
                    if (bits >> (4 - rx)) & 1:
                        for sy in range(scale):
                            for sx in range(scale):
                                set_px(cx + rx * scale + sx, y0 + ry * scale + sy, black)
            cx += 6 * scale

    # White text on black banner
    draw_text("DISPLIO", 48, 22, 3, black=False)
    # Black text below
    draw_text("REBIRTH V1.0", 24, 72, 3, black=True)

    # Checkerboard strip at bottom
    for y in range(118, 160):
        for x in range(16, EPD_W - 16):
            if (((x - 16) // 8) + ((y - 118) // 8)) % 2 == 0:
                set_px(x, y, True)

    return fb


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dev", help="hidraw device path")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info", help="query custom firmware info")

    bp = sub.add_parser("beep", help="play a tone on the piezo buzzer")
    bp.add_argument("--freq", type=int, default=2400)
    bp.add_argument("--ms", type=int, default=80)

    et = sub.add_parser("epd-test", help="upload and display the DISPLIO REBIRTH test card")
    et.add_argument("--full", action="store_true", help="run full black/white flash clear first")

    sub.add_parser("scan", help="run I2C, GPIO, ADC, and SPI2 bus scans")
    sub.add_parser("bootloader", help="reboot cleanly back into the USB HID bootloader")

    a = p.parse_args(argv)
    try:
        with DisplioApp(a.dev) as app:
            if a.cmd == "info":
                info = app.info()
                print(f"device    : {app.path}")
                for k, v in info.items():
                    print(f"{k:10s}: {v}")
            elif a.cmd == "beep":
                app.beep(a.freq, a.ms)
                print(f"beeped {a.freq} Hz for {a.ms} ms")
            elif a.cmd == "epd-test":
                fb = make_test_pattern()
                t0 = time.time()
                app.send_fb(bytes(fb))
                print(f"framebuffer uploaded ({len(fb)} B in {time.time() - t0:.2f}s), refreshing EPD...")
                t1 = time.time()
                app.refresh(full_clear=a.full)
                print(f"EPD refresh complete ({time.time() - t1:.2f}s)")
            elif a.cmd == "scan":
                print("=== I2C1 (PB6/PB7) ===")
                for addr, r0f, r0d, r00 in app.scan_i2c(0):
                    print(f"  0x{addr:02x}: reg0F=0x{r0f:02x} reg0D=0x{r0d:02x} reg00=0x{r00:02x}")
                print("=== I2C1 remap (PB8/PB9) ===")
                for addr, r0f, r0d, r00 in app.scan_i2c(1):
                    print(f"  0x{addr:02x}: reg0F=0x{r0f:02x} reg0D=0x{r0d:02x} reg00=0x{r00:02x}")
                print("=== GPIO IDR & ADC1 ===")
                idr, adc = app.scan_io()
                print(f"  GPIOA=0x{idr[0]:04x}  GPIOB=0x{idr[1]:04x}  GPIOC=0x{idr[2]:04x}")
                vref = adc[10]
                vdda = (1.20 * 4095.0 / vref) if vref else 3.3
                print(f"  Vrefint={vref} (~VDDA={vdda:.2f}V)")
                for ch in range(10):
                    v = adc[ch] * vdda / 4095.0
                    pin = f"PA{ch}" if ch < 8 else f"PB{ch - 8}"
                    print(f"  ADC CH{ch} ({pin}): {adc[ch]:4d} ({v:.2f}V)")
                print("=== SPI2 JEDEC 0x9F ===")
                for name, b0, b1, b2 in app.scan_spi2():
                    print(f"  CS={name}: {b0:02x} {b1:02x} {b2:02x}")
            elif a.cmd == "bootloader":
                app.enter_bootloader()
                print("rebooting into USB HID bootloader...")
        if a.cmd == "bootloader":
            path = wait_for_device(10.0, gone_first=1.0)
            print(f"bootloader ready on {path}")
    except (DisplioError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
