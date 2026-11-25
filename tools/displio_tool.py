#!/usr/bin/env python3
"""displio-tool — host utility for the Displio V1.0 USB HID bootloader (0483:a197).

Protocol summary (see docs/protocol.md for details):
  * 64-byte HID reports, no report ID.
  * byte 0     : sequence/direction  ((seq << 1) | 1 host->device, (seq << 1) device->host)
  * byte 1     : command (request) or status (response)
  * bytes 2-61 : payload, zero padded
  * bytes 62-63: CRC-16/EN-13757 (poly 0x3D65, init 0, xorout 0xFFFF), little-endian

Only the Python standard library is used; Linux hidraw is required.
"""
from __future__ import annotations

import argparse
import glob
import os
import select
import struct
import sys
import time

VID, PID = 0x0483, 0xA197

APP_START = 0x08004000
APP_END = 0x08020000          # exclusive
MAX_WORDS = 13                # 13 * 4 = 52 data bytes per packet

CMD_SYNC = 0x01
CMD_GET_INFO = 0x02
CMD_ERASE_APP = 0x03
CMD_WRITE = 0x04
CMD_READ = 0x05
CMD_FINALIZE = 0x06
CMD_BOOT_DELAYED = 0x07       # device disconnects after ~3 s (exact semantics TBC)
CMD_REBOOT = 0x08             # device disconnects immediately (exact semantics TBC)

ST_ERROR = 0x04
ERRORS = {
    0x01: "sequence error",
    0x02: "invalid word count (must be 1..13)",
    0x03: "address above flash end (>= 0x08020000)",
    0x04: "address inside protected bootloader (< 0x08004000)",
    0x05: "application verification failed",
    0x08: "unknown command",
}


class DisplioError(Exception):
    pass


def crc16_en13757(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x3D65) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc ^ 0xFFFF


def stm32_crc32(data: bytes) -> int:
    """STM32 hardware CRC-32 (poly 0x04C11DB7, init 0xFFFFFFFF, u32 LE words)."""
    if len(data) % 4:
        data = data + b"\xff" * (-len(data) % 4)
    crc = 0xFFFFFFFF
    for i in range(0, len(data), 4):
        crc ^= struct.unpack_from("<I", data, i)[0]
        for _ in range(32):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def find_hidraw() -> str:
    for path in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            with open(os.path.join(path, "device", "uevent")) as f:
                if f"HID_ID=0003:{VID:08X}:{PID:08X}" in f.read():
                    return "/dev/" + os.path.basename(path)
        except OSError:
            continue
    raise DisplioError("Displio (0483:a197) not found — is it plugged in?")


class Displio:
    def __init__(self, path: str | None = None, verbose: bool = False):
        self.path = path or find_hidraw()
        self.verbose = verbose
        self.fd = os.open(self.path, os.O_RDWR | os.O_NONBLOCK)
        self.seq = 0
        self._drain()

    def close(self):
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- transport -------------------------------------------------------
    def _drain(self):
        while select.select([self.fd], [], [], 0.01)[0]:
            os.read(self.fd, 64)

    def _xfer(self, b0: int, cmd: int, payload: bytes = b"", timeout: float = 0.5) -> bytes:
        body = bytes([b0, cmd]) + payload
        body = body.ljust(62, b"\x00")[:62]
        pkt = body + struct.pack("<H", crc16_en13757(body))
        if self.verbose:
            print(f"  > {pkt[:16].hex()}", file=sys.stderr)
        os.write(self.fd, b"\x00" + pkt)
        if not select.select([self.fd], [], [], timeout)[0]:
            raise DisplioError(f"timeout waiting for reply to cmd 0x{cmd:02x}")
        resp = os.read(self.fd, 64)
        if self.verbose:
            print(f"  < {resp[:16].hex()}", file=sys.stderr)
        if len(resp) != 64 or crc16_en13757(resp[:62]) != struct.unpack("<H", resp[62:64])[0]:
            raise DisplioError(f"bad reply CRC: {resp.hex()}")
        if resp[1] == ST_ERROR:
            code = resp[2]
            raise DisplioError(f"device error 0x{code:02x}: {ERRORS.get(code, 'unknown')}")
        if resp[1] == 0x09 and resp[2] == 0x0A:
            raise DisplioError("device reported request CRC error")
        return resp

    def sync(self):
        """Reset the device-side sequence counter. Next command uses seq 0."""
        self._xfer(0x01, CMD_SYNC)
        self.seq = 0

    def command(self, cmd: int, payload: bytes = b"", timeout: float = 0.5) -> bytes:
        b0 = ((self.seq & 0x7F) << 1) | 1
        resp = self._xfer(b0, cmd, payload, timeout)
        self.seq += 1
        return resp

    # ---- high level ------------------------------------------------------
    def get_info(self) -> bytes:
        self.sync()
        return self.command(CMD_GET_INFO)[2:9]

    def read(self, addr: int, length: int, progress=None) -> bytes:
        _check_range(addr, length)
        self.sync()
        out = bytearray()
        end = addr + length
        while addr < end:
            words = min(MAX_WORDS, (end - addr + 3) // 4)
            resp = self.command(CMD_READ, struct.pack("<BI", words, addr))
            if resp[1] != CMD_READ or resp[2] != words or struct.unpack("<I", resp[3:7])[0] != addr:
                raise DisplioError(f"unexpected read reply at 0x{addr:08x}: {resp[:12].hex()}")
            out += resp[7:7 + words * 4]
            addr += words * 4
            if progress:
                progress(len(out), length)
        return bytes(out[:length])

    def write(self, addr: int, data: bytes, progress=None):
        if addr % 4:
            raise DisplioError("write address must be 4-byte aligned")
        data = data + b"\xff" * (-len(data) % 4)
        _check_range(addr, len(data))
        self.sync()
        done = 0
        while done < len(data):
            chunk = data[done:done + MAX_WORDS * 4]
            words = len(chunk) // 4
            resp = self.command(CMD_WRITE, struct.pack("<BI", words, addr + done) + chunk)
            if resp[1] != 0x02:
                raise DisplioError(f"unexpected write reply at 0x{addr + done:08x}: {resp[:12].hex()}")
            done += len(chunk)
            if progress:
                progress(done, len(data))

    def erase_app(self):
        self.sync()
        resp = self.command(CMD_ERASE_APP, timeout=10.0)
        if resp[1] != 0x02:
            raise DisplioError(f"unexpected erase reply: {resp[:12].hex()}")

    def validate(self):
        self.sync()
        resp = self.command(CMD_FINALIZE, timeout=3.0)
        if resp[1] != 0x02:
            raise DisplioError(f"unexpected validate reply: {resp[:12].hex()}")

    def flash_app(self, image: bytes, boot: bool = False, progress=None, preserve_wifi: bool = True):
        """Erase, write image + STM32 CRC-32 trailer at 0x0801FFFC, verify, validate, optionally boot.

        By default preserves the saved Wi-Fi config block at 0x0801F800 across updates.
        """
        wifi_addr = 0x0801F800
        wifi_len = 128
        max_len = (APP_END - APP_START) - 4
        if len(image) > max_len:
            raise DisplioError(f"image too large ({len(image)} > {max_len} bytes)")
        padded = image + b"\xff" * (-len(image) % 4)

        saved_wifi: bytes | None = None
        if preserve_wifi and len(padded) <= (wifi_addr - APP_START):
            try:
                blob = self.read(wifi_addr, wifi_len)
                if blob[:4] in (b"2FIW", b"IFIW"):
                    saved_wifi = blob
                    print(f"preserving saved Wi-Fi config at 0x{wifi_addr:08x}...", file=sys.stderr)
            except (DisplioError, OSError):
                saved_wifi = None

        full = bytearray(padded + b"\xff" * (max_len - len(padded)))
        if saved_wifi is not None:
            off = wifi_addr - APP_START
            full[off:off + len(saved_wifi)] = saved_wifi
        crc = stm32_crc32(bytes(full))
        print("erasing...", file=sys.stderr)
        self.erase_app()
        print(f"writing {len(padded)} bytes + CRC 0x{crc:08x} at 0x{APP_END - 4:08x}...", file=sys.stderr)
        self.write(APP_START, padded, progress)
        if saved_wifi is not None:
            print(f"restoring Wi-Fi config at 0x{wifi_addr:08x}...", file=sys.stderr)
            self.write(wifi_addr, saved_wifi)
        self.write(APP_END - 4, struct.pack("<I", crc))
        print("verifying...", file=sys.stderr)
        back = self.read(APP_START, len(padded), progress)
        if back != padded:
            bad = next(i for i in range(len(padded)) if back[i] != padded[i])
            raise DisplioError(f"verify failed at 0x{APP_START + bad:08x}")
        print("validating (cmd 0x06)...", file=sys.stderr)
        self.validate()
        if boot:
            print("starting application (cmd 0x07)...", file=sys.stderr)
            self.reboot(CMD_BOOT_DELAYED)

    def reboot(self, cmd: int = CMD_REBOOT) -> bytes | None:
        """Send a command that makes the device re-enumerate; tolerate disconnect."""
        self.sync()
        try:
            return self.command(cmd, timeout=4.0)
        except (DisplioError, OSError):
            return None


def wait_for_device(timeout: float = 10.0, gone_first: float = 0.0) -> str:
    deadline = time.time() + timeout
    time.sleep(gone_first)
    while time.time() < deadline:
        try:
            path = find_hidraw()
            os.close(os.open(path, os.O_RDWR))
            return path
        except (DisplioError, OSError):
            time.sleep(0.2)
    raise DisplioError("device did not come back")


def _check_range(addr: int, length: int):
    if addr < APP_START or addr + length > APP_END or length <= 0:
        raise DisplioError(
            f"range 0x{addr:08x}+{length} outside application area "
            f"0x{APP_START:08x}..0x{APP_END:08x}")


def _progress(done: int, total: int, _last=[-1]):
    pct = 100 * done // total
    if pct == _last[0] and done < total:
        return
    _last[0] = pct
    print(f"\r  {done}/{total} bytes ({pct}%)", end="", file=sys.stderr, flush=True)
    if done >= total:
        print(file=sys.stderr)


def _int(s: str) -> int:
    return int(s, 0)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--dev", help="hidraw device (auto-detected by default)")
    p.add_argument("-v", "--verbose", action="store_true", help="print raw packets")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info", help="read bootloader info")

    r = sub.add_parser("read", help="hexdump a flash range")
    r.add_argument("addr", type=_int)
    r.add_argument("length", type=_int)

    d = sub.add_parser("dump", help="dump the whole application area to a file")
    d.add_argument("output")

    e = sub.add_parser("erase", help="erase the whole application area (DESTRUCTIVE)")
    e.add_argument("--yes", action="store_true", help="confirm")

    w = sub.add_parser("write", help="erase, write and verify a binary (DESTRUCTIVE)")
    w.add_argument("input")
    w.add_argument("--addr", type=_int, default=APP_START)
    w.add_argument("--no-erase", action="store_true")
    w.add_argument("--yes", action="store_true", help="confirm")

    fl = sub.add_parser("flash", help="erase, write, append STM32 CRC-32, verify and validate (DESTRUCTIVE)")
    fl.add_argument("input")
    fl.add_argument("--boot", action="store_true", help="launch application (cmd 0x07) after validation")
    fl.add_argument("--wipe-wifi", action="store_true", help="do not preserve saved Wi-Fi config at 0x0801F800")
    fl.add_argument("--yes", action="store_true", help="confirm")

    rb = sub.add_parser("reboot", help="make the device re-enumerate (cmd 0x08, or 0x07)")
    rb.add_argument("--opcode", type=_int, default=CMD_REBOOT, choices=[CMD_BOOT_DELAYED, CMD_REBOOT])
    a = p.parse_args(argv)
    try:
        with Displio(a.dev, a.verbose) as dev:
            if a.cmd == "info":
                info = dev.get_info()
                print(f"device       : {dev.path}")
                print(f"info raw     : {info.hex()}")
                print(f"bootloader   : v{info[1]}.{info[2]} (interpretation tentative)")
                print(f"app state    : {info[3:7].hex()} (ffffffff = erased)")
            elif a.cmd == "read":
                data = dev.read(a.addr, a.length)
                for off in range(0, len(data), 16):
                    row = data[off:off + 16]
                    asc = "".join(chr(c) if 32 <= c < 127 else "." for c in row)
                    print(f"{a.addr + off:08x}  {row.hex(' '):47s}  {asc}")
            elif a.cmd == "dump":
                t0 = time.time()
                data = dev.read(APP_START, APP_END - APP_START, _progress)
                with open(a.output, "wb") as f:
                    f.write(data)
                used = len(data.rstrip(b"\xff"))
                print(f"wrote {a.output} ({len(data)} bytes, {used} bytes before trailing 0xFF, "
                      f"{time.time() - t0:.1f}s)")
            elif a.cmd == "erase":
                if not a.yes:
                    raise DisplioError("refusing to erase without --yes")
                dev.erase_app()
                print("application area erased")
            elif a.cmd == "write":
                if not a.yes:
                    raise DisplioError("refusing to write without --yes")
                with open(a.input, "rb") as f:
                    data = f.read()
                if not a.no_erase:
                    print("erasing...", file=sys.stderr)
                    dev.erase_app()
                print("writing...", file=sys.stderr)
                dev.write(a.addr, data, _progress)
                print("verifying...", file=sys.stderr)
                back = dev.read(a.addr, len(data), _progress)
                if back != data:
                    bad = next(i for i in range(len(data)) if back[i] != data[i])
                    raise DisplioError(f"verify failed at 0x{a.addr + bad:08x}")
                print(f"OK: {len(data)} bytes written and verified at 0x{a.addr:08x}")
                print("note: use 'flash' to append CRC-32 and validate (cmd 0x06)")
            elif a.cmd == "flash":
                if not a.yes:
                    raise DisplioError("refusing to flash without --yes")
                with open(a.input, "rb") as f:
                    data = f.read()
                dev.flash_app(data, boot=a.boot, progress=_progress, preserve_wifi=not a.wipe_wifi)
                print(f"OK: {len(data)} bytes flashed and validated" + (" (booting)" if a.boot else ""))
            elif a.cmd == "reboot":
                resp = dev.reboot(a.opcode)
                print(f"reply: {resp[:12].hex() if resp else 'none (disconnected)'}")
        if a.cmd == "reboot":
            t0 = time.time()
            path = wait_for_device(15.0, gone_first=1.0)
            print(f"device back on {path} after ~{time.time() - t0 + 1:.1f}s")
    except (DisplioError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
