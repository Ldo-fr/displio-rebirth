# Displio V1.0 — USB HID bootloader protocol

Reverse-engineered from the device over USB (box closed). Status flags:
**[confirmed]** tested and reproducible, **[tentative]** inferred, not fully verified.

## Device

| Item | Value |
|---|---|
| USB ID | `0483:a197`, manufacturer `DISPLIO`, product `DISPLIO V1.0` |
| Speed | Full speed (12 Mbit/s), bus powered + self powered |
| Interface | 1 × HID, vendor usage page `0xFF00` |
| Endpoints | `0x81` IN / `0x01` OUT, interrupt, 64 bytes, `bInterval` 32 |
| Reports | 64-byte input + 64-byte output, **no report ID** |
| MCU | STM32 (vendor ID 0483), 128 KiB flash at `0x08000000` **[tentative]** |

On Linux the device shows up as `/dev/hidrawN`; writes must be prefixed by a
`0x00` report-ID byte (65 bytes written, 64 sent on the wire).

## Flash map

| Range | Content | Access |
|---|---|---|
| `0x08000000`–`0x08003FFF` (16 KiB) | Bootloader | read/write refused (error `0x04`) **[confirmed]** |
| `0x08004000`–`0x0801FFFF` (112 KiB) | Application | read/write/erase **[confirmed]** |
| `≥ 0x08020000` | — | refused (error `0x03`) **[confirmed]** |

## Packet format (both directions) **[confirmed]**

| Bytes | Field | Notes |
|---|---|---|
| 0 | seq/dir | host → device: `(seq << 1) \| 1`; device → host: even value |
| 1 | command / status | |
| 2–61 | payload | zero padded |
| 62–63 | CRC | CRC-16/EN-13757 over bytes 0–61, little-endian |

CRC-16/EN-13757: poly `0x3D65`, init `0x0000`, no reflection, xorout `0xFFFF`.

```python
def crc16_en13757(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x3D65) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc ^ 0xFFFF
```

A request with a bad CRC gets the reply `xx 09 0a 00…`.

### Sequence numbers **[partially understood]**

* `SYNC` (`0x01`) sent with byte 0 = `0x01` always succeeds and resets the counter.
* After `SYNC`, streaming commands (`READ`, `WRITE`) are accepted with byte 0 =
  `0x01, 0x03, 0x05, …` (seq 0, 1, 2, …). Tested over > 2 000 consecutive packets.
* Re-sending the same byte 0 as the previous packet seems to return the previous
  reply again (retransmission) **[tentative]**.
* Wrong sequence → error `0x01`.

The host tool always sends `SYNC` before each logical operation.

## Commands

| Cmd | Name | Request payload | Reply |
|---|---|---|---|
| `0x01` | SYNC | — | `00 02 00…` **[confirmed]** |
| `0x02` | GET_INFO | — | `xx 01 00 01 01 SS SS SS SS` — `01 01` = bootloader version 1.1 **[tentative]**; `SS` = `ffffffff` after erase, `ffff0000` after any write **[observed]** |
| `0x03` | ERASE_APP | — | `xx 02 00…` after ~2.5 s; erases the whole 112 KiB application area **[confirmed]** |
| `0x04` | WRITE | `N:u8 (1..13)`, `addr:u32le`, `N × u32` | `xx 02 00…` **[confirmed]** |
| `0x05` | READ | `N:u8 (1..13)`, `addr:u32le` | `xx 05 N addr:u32le data[N×4]` **[confirmed]** |
| `0x06` | FINALIZE / VERIFY | unknown | with an empty/garbage app → error `0x05` **[tentative]** |
| `0x07` | ? | — | no reply, device re-enumerates after ~3 s **[observed]** |
| `0x08` | ? | — | device re-enumerates immediately **[observed]** |
| other | — | — | error `0x08` |

Throughput: about 25 KiB/s for reads (52 bytes per round-trip).

## Error replies (`xx 04 CC…`)

| Code | Meaning |
|---|---|
| `0x01` | sequence error |
| `0x02` | invalid word count (must be 1..13) |
| `0x03` | address ≥ `0x08020000` |
| `0x04` | address < `0x08004000` (bootloader protected) |
| `0x05` | application verification failed (`0x06`) |
| `0x08` | unknown command |

## Open questions

1. Parameters expected by `0x06` and what it validates (CRC? size? magic?).
2. Does the bootloader jump to the application automatically at reset when a
   valid vector table is present, or only after `0x06`/`0x07`?
3. What brings the device back into the bootloader once an application runs
   (USB plugged at reset? magic word in RAM? timeout?) — critical to avoid bricking.
4. Exact STM32 part number, clock and pin mapping (display, accelerometer, Wi-Fi,
   buzzer, battery).

