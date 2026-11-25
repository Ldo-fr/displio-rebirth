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
CMD_ACCEL = 0x20
CMD_I2C_XFER = 0x21
CMD_GPIO_CTRL = 0x22
CMD_SPI2_XFER = 0x23
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

    def accel(self) -> tuple[int, int, int, int]:
        d = self.cmd(CMD_ACCEL)
        x, y, z, pl = struct.unpack_from("<hhhB", d, 0)
        return x, y, z, pl

    def i2c_xfer(self, addr: int, tx: bytes, rx_len: int = 0, wait_ms: int = 0) -> bytes:
        d = self.cmd(CMD_I2C_XFER, struct.pack("<BBBB", addr, len(tx), rx_len, wait_ms) + tx,
                     timeout=max(1.0, wait_ms / 1000.0 + 0.5))
        return d[:rx_len]

    def gpio_ctrl(self, port: int, pin: int, mode: int, val: int, wait_ms: int = 50) -> tuple[list[int], list[int]]:
        d = self.cmd(CMD_GPIO_CTRL, struct.pack("<BBBBB", port, pin, mode, val, wait_ms),
                     timeout=max(1.0, wait_ms / 1000.0 + 0.5))
        vals = list(struct.unpack_from("<6H", d, 0))
        return vals[:3], vals[3:]

    def spi2_xfer(self, cs_pp: int, tx: bytes, mode: int = 0, br_div: int = 3) -> bytes:
        d = self.cmd(CMD_SPI2_XFER, struct.pack("<BBBB", cs_pp, mode, br_div, len(tx)) + tx)
        return d[:len(tx)]


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
    sub.add_parser("sensors", help="read MMA8452Q accelerometer, Si7021 temp/RH, and battery voltage")
    sub.add_parser("wifi-info", help="boot TI CC3000 over SPI2 and read Service Pack version")
    sub.add_parser("wifi-probe", help="probe candidate Wi-Fi power/reset/IRQ pins and SPI2")
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
            elif a.cmd == "sensors":
                x, y, z, pl = app.accel()
                orient_map = {0: "Portrait Up", 1: "Portrait Down", 2: "Landscape Right", 3: "Landscape Left"}
                orient = orient_map.get((pl >> 1) & 3, "?")
                face = "Back" if (pl & 1) else "Front"
                print(f"=== MMA8452Q (0x1C) ===")
                print(f"  X={x:+5d} mg  Y={y:+5d} mg  Z={z:+5d} mg  PL=0x{pl:02x} ({orient}, {face})")
                print(f"=== I2C 0x40 Sensor Probe ===")
                # Try Si7021 / HTU21D / SHT21
                try:
                    ureg = app.i2c_xfer(0x40, b"\xe7", 1)
                    print(f"  0xE7 (User Reg): {ureg.hex()}")
                except DisplioError as e:
                    print(f"  0xE7 failed: {e}")
                try:
                    t_raw = app.i2c_xfer(0x40, b"\xf3", 3, wait_ms=40)
                    raw_t = ((t_raw[0] << 8) | t_raw[1]) & 0xFFFC
                    temp_c = -46.85 + 175.72 * raw_t / 65536.0
                    print(f"  0xF3 (Temp No-Hold): {t_raw.hex()} -> {temp_c:.2f} °C")
                except DisplioError as e:
                    print(f"  0xF3 failed: {e}")
                try:
                    h_raw = app.i2c_xfer(0x40, b"\xf5", 3, wait_ms=40)
                    raw_h = ((h_raw[0] << 8) | h_raw[1]) & 0xFFFC
                    rh = -6.0 + 125.0 * raw_h / 65536.0
                    print(f"  0xF5 (RH No-Hold)  : {h_raw.hex()} -> {rh:.1f} %")
                except DisplioError as e:
                    print(f"  0xF5 failed: {e}")
                try:
                    # Si7021 electronic ID / firmware rev (0x84 0xB8) or HDC1080 mfg ID (0xFE/0xFF)
                    fw = app.i2c_xfer(0x40, b"\x84\xb8", 1)
                    print(f"  Si7021 FW rev (0x84B8): {fw.hex()}")
                except DisplioError:
                    pass
                for reg in (0xFE, 0xFF):
                    try:
                        v = app.i2c_xfer(0x40, bytes([reg]), 2)
                        print(f"  reg 0x{reg:02x}: {v.hex()}")
                    except DisplioError:
                        pass
                # Battery voltage via PC13 -> PA0
                app.gpio_ctrl(2, 13, 0x03, 1, wait_ms=30)
                _, adc = app.scan_io()
                app.gpio_ctrl(2, 13, 0x03, 0, wait_ms=10)
                vdda = (1.20 * 4095.0 / adc[10]) if adc[10] else 3.0
                v_pa0 = adc[0] * vdda / 4095.0
                print(f"=== Power / Battery (PC13 -> PA0) ===")
                print(f"  VDDA={vdda:.2f}V  PA0={adc[0]} ({v_pa0:.3f}V raw, ~{v_pa0*1.8:.2f}V..{v_pa0*2.0:.2f}V VBAT)")
            elif a.cmd == "wifi-info":
                print("=== TI CC3000 Wi-Fi (SPI2: EN=PB12, CS=PB4, IRQ=PA8) ===")
                app.gpio_ctrl(0, 8, 0x04, 0, wait_ms=5)
                app.gpio_ctrl(1, 4, 0x03, 1, wait_ms=5)
                app.gpio_ctrl(1, 12, 0x03, 0, wait_ms=50)
                app.spi2_xfer(0xFF, b"\x00", mode=1, br_div=4)
                _, a1 = app.gpio_ctrl(1, 12, 0x03, 1, wait_ms=100)
                if ((a1[0] >> 8) & 1) != 0:
                    raise DisplioError("CC3000 did not assert IRQ LOW after power-on")
                # FIRST_WRITE: HCI_CMND_SIMPLE_LINK_START (0x4000)
                app.gpio_ctrl(1, 4, 0x03, 0, wait_ms=1)
                app.spi2_xfer(0xFF, bytes([0x01, 0x00, 0x05, 0x00]), mode=1, br_div=4)
                time.sleep(0.001)
                app.spi2_xfer(0xFF, bytes([0x00, 0x01, 0x00, 0x40, 0x01, 0x00]), mode=1, br_div=4)
                app.gpio_ctrl(1, 4, 0x03, 1, wait_ms=5)

                def cc_read(timeout_s=1.5):
                    deadline = time.time() + timeout_s
                    while time.time() < deadline:
                        idr, _ = app.scan_io()
                        if ((idr[0] >> 8) & 1) == 0:
                            app.gpio_ctrl(1, 4, 0x03, 0, wait_ms=1)
                            hdr = app.spi2_xfer(0xFF, bytes([0x03, 0x00, 0x00, 0x00, 0x00]), mode=1, br_div=4)
                            plen = (hdr[3] << 8) | hdr[4]
                            pay = app.spi2_xfer(0xFF, b"\x00" * min(plen, 40), mode=1, br_div=4) if plen else b""
                            app.gpio_ctrl(1, 4, 0x03, 1, wait_ms=5)
                            return hdr, pay
                        time.sleep(0.01)
                    return None, None

                hdr, pay = cc_read(1.5)
                print(f"  SIMPLE_LINK_START : {pay.hex() if pay else 'timeout'}")
                # HCI_CMND_READ_SP_VERSION (0x4007)
                app.gpio_ctrl(1, 4, 0x03, 0, wait_ms=1)
                for _ in range(30):
                    idr, _ = app.scan_io()
                    if ((idr[0] >> 8) & 1) == 0:
                        break
                    time.sleep(0.005)
                app.spi2_xfer(0xFF, bytes([0x01, 0x00, 0x05, 0x00, 0x00, 0x01, 0x07, 0x40, 0x00, 0x00]), mode=1, br_div=4)
                app.gpio_ctrl(1, 4, 0x03, 1, wait_ms=5)
                hdr, pay = cc_read(1.5)
                if pay and len(pay) >= 7:
                    print(f"  READ_SP_VERSION   : {pay.hex()} -> Package {pay[5]}.{pay[6]}")
                else:
                    print(f"  READ_SP_VERSION   : {pay.hex() if pay else 'timeout'}")
                app.gpio_ctrl(1, 12, 0x03, 0, wait_ms=10)
            elif a.cmd == "wifi-probe":
                print("=== Probing candidate power/enable/reset pins ===")
                # Candidate pins: (port 0=A,1=B,2=C, pin, name)
                cands = [
                    (1, 4, "PB4"), (1, 12, "PB12"), (2, 13, "PC13"),
                    (0, 8, "PA8"), (0, 15, "PA15"), (1, 3, "PB3"),
                    (1, 5, "PB5"), (1, 8, "PB8"), (1, 9, "PB9"),
                ]
                for port, pin, name in cands:
                    for val in (1, 0):
                        before, after = app.gpio_ctrl(port, pin, 0x03, val, wait_ms=100)
                        # Mask out the pin we just drove
                        diffs = []
                        for pi, pn in enumerate("ABC"):
                            d = before[pi] ^ after[pi]
                            if pi == port:
                                d &= ~(1 << pin)
                            if d:
                                bits = [f"P{pn}{b}({'1' if (after[pi]>>b)&1 else '0'})" for b in range(16) if (d>>b)&1]
                                diffs.extend(bits)
                        _, adc = app.scan_io()
                        if diffs or adc[0] > 50:
                            print(f"  [!] {name}={val} -> input changes: {diffs}  ADC_PA0={adc[0]}")
                        else:
                            print(f"      {name}={val} -> no other pin change (GPIOA={after[0]:04x} GPIOB={after[1]:04x})")
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
