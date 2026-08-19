# In-App Firmware Update (IAP) Protocol

This document describes the raw HID protocol used to update the ReLow60 L-HE
firmware from a WebHID host (ReConf) without entering the ROM DFU bootloader.
No WinUSB driver (Zadig) or WebUSB access is required on Windows.

Reference implementation:

- Firmware: `firmware/include/iap.h`, `firmware/src/iap.c`,
  `firmware/src/hardware/at32f405xx/iap.c`
- Image generation (build): `firmware/scripts/iap_image.py`
- Host (ReConf): `src/lib/iap/` and `src/lib/libhmk/commands/fw-update.ts`

IAP transfers use the dedicated image `firmware_iap.bin` (raw application
binary plus a 16-byte self-verification trailer, see below). The plain
`firmware.bin` (DFU flashing path) has no trailer and is rejected by VERIFY.

## Memory map

The AT32F405RCT7 has 256KB of single-bank flash (128 sectors x 2KB).

| Range                     | Size | Use                                  |
| ------------------------- | ---- | ------------------------------------ |
| `0x08000000 - 0x08017000` | 92KB | Application (capped by linker)       |
| `0x08017000 - 0x0802E000` | 92KB | IAP staging area                     |
| `0x0802E000 - 0x08040000` | 72KB | Wear leveling backing store (eeconfig) |

A new image is streamed into the staging area, verified there, and then the
payload (the image minus its trailer) is copied over the application region
by a RAM-resident routine followed by a system reset. The trailer is never
written to the application region; it remains in the staging area, so it
does not count against the 92KB application cap. Images larger than 92KB
(payload + trailer) are rejected at INIT.

Because the flash is single-bank, instruction fetch stalls while the flash
controller erases or programs:

- Staging writes are tolerable (sub-millisecond word programming, one 2KB
  sector erase on demand as each sector boundary is crossed).
- Rewriting the application itself must run from RAM with interrupts
  disabled; USB does not respond during this phase (roughly 1-3 seconds) and
  the device then re-enumerates.

## Image self-verification trailer

### Why a host-computed CRC is not sufficient

The CRC given at INIT is computed by the host over the file the user
selected. If that file was corrupted *before* the host read it (bad
download, truncated copy, wrong file), the host and the device both hash
the same corrupted bytes: the CRCs match and VERIFY passes. This was
reproduced on hardware — a firmware file with a single flipped byte in the
middle sailed through VERIFY (the vector table at the head was intact) and
was applied.

The host-supplied CRC therefore only proves **transfer integrity**
(host-to-device bytes arrived intact). **Image validity** — "this file is a
correct ReLow60 firmware image" — must come from information embedded in
the image itself at build time. That is the trailer's job.

### Trailer format

`firmware_iap.bin` = `[payload][trailer]`, where the payload is the raw
application binary (identical to `firmware.bin` before the DFU suffix is
added) and the trailer is 16 bytes, little-endian:

| Offset | Size | Field                                                  |
| ------ | ---- | ------------------------------------------------------ |
| 0      | 4    | `magic` — `0x52424649` ("IFBR" as a LE string)         |
| 4      | 2    | `version` — `FIRMWARE_VERSION` of the payload          |
| 6      | 2    | `reserved` — 0                                         |
| 8      | 4    | `payload_len` — payload size in bytes                  |
| 12     | 4    | `payload_crc32` — CRC32 of the payload (see below)     |

The trailer is appended by the post-build script
`firmware/scripts/iap_image.py`, which runs after the platform builder adds
the DFU suffix to `firmware.bin`. The script strips that DFU suffix first,
so the trailer vouches for the pure application payload:

- `firmware.bin` = payload + DFU suffix — for dfu-util / WebUSB DFU,
  unchanged.
- `firmware_iap.bin` = payload + IAP trailer — for the IAP path only.

The `payload_crc32` uses the same AT32 CRC32 definition as the rest of the
protocol (see "CRC32 definition"), with an initial argument of 0. The
script self-tests its CRC implementation against the test vectors below on
every build.

The `version` field is informational only: the firmware does **not** block
downgrades, since rolling back to an older release is a supported use case.

## Transport

The existing 64-byte raw HID report interface is used (usage page `0xFFAB`,
usage `0xAB`, report length exactly 64 bytes, little-endian integers).
Byte 0 of a request is the command ID; byte 0 of a response echoes it.

Command IDs (extending the existing enum, no existing value changes):

| Command                  | ID  |
| ------------------------ | --- |
| `COMMAND_FW_UPDATE_INIT`   | 18  |
| `COMMAND_FW_UPDATE_WRITE`  | 19  |
| `COMMAND_FW_UPDATE_VERIFY` | 20  |
| `COMMAND_FW_UPDATE_APPLY`  | 21  |

Unlike older commands (which reply with `COMMAND_UNKNOWN` = 255 on failure),
the IAP commands **always echo the command ID** and report failures through a
status byte at offset 1 of the response. This keeps the host's
request/response matching simple.

## Status codes

| Value | Name                      | Meaning                                            |
| ----- | ------------------------- | -------------------------------------------------- |
| 0     | `IAP_STATUS_OK`           | Success                                            |
| 1     | `IAP_STATUS_ERR_STATE`    | Command not valid in the current state             |
| 2     | `IAP_STATUS_ERR_SIZE`     | Image size < 528 bytes (512 min payload + 16 trailer) or > staging capacity |
| 3     | `IAP_STATUS_ERR_OFFSET`   | WRITE offset != expected sequential offset         |
| 4     | `IAP_STATUS_ERR_LENGTH`   | WRITE length invalid (0, > chunk size, past image end, or non-final chunk not a multiple of 4) |
| 5     | `IAP_STATUS_ERR_ERASE`    | Staging sector erase failed (transfer aborted)     |
| 6     | `IAP_STATUS_ERR_WRITE`    | Staging flash write failed (transfer aborted)      |
| 7     | `IAP_STATUS_ERR_CRC`      | Transfer integrity failure: staged image CRC32 does not match the INIT value (transfer aborted) |
| 8     | `IAP_STATUS_ERR_VECTOR`   | Staged payload vector table invalid (transfer aborted) |
| 9     | `IAP_STATUS_ERR_MAGIC`    | APPLY magic mismatch                               |
| 10    | `IAP_STATUS_ERR_GEOMETRY` | Staging base not aligned to a flash sector         |
| 11    | `IAP_STATUS_ERR_IMAGE`    | Image self-verification failure: trailer magic missing, `payload_len` inconsistent, or payload CRC32 does not match the trailer (transfer aborted) |

`ERR_CRC` means the bytes were damaged **between** host and device (retry
may help); `ERR_IMAGE` means the file itself is not a valid IAP image — it
has no trailer (e.g. a plain `firmware.bin`), or it was corrupted before
the host read it (retrying with the same file will fail again).

"Transfer aborted" means the state machine returns to idle; the host must
start over with a new INIT.

## State machine

```
IDLE --INIT ok--> RECEIVING --last WRITE ok--> RECEIVED --VERIFY ok--> VERIFIED
                                                                        |
                                              APPLY ok --> APPLYING ----+
                                                              |
                            (~250ms later: IRQs off, RAM routine, reset)
```

- `INIT` is accepted in any state except `APPLYING` and always restarts the
  transfer from scratch.
- `VERIFY` may be repeated while in `RECEIVED`/`VERIFIED`.
- Any hard error (erase/write/CRC/vector) drops back to `IDLE`.

## Report formats

All integers are little-endian. Offsets below are byte offsets in the
64-byte report. Unlisted trailing bytes are padding (send as zero, ignore on
receive).

### COMMAND_FW_UPDATE_INIT (18)

Request:

| Offset | Size | Field                          |
| ------ | ---- | ------------------------------ |
| 0      | 1    | Command ID (18)                |
| 1      | 4    | `size` — total image size in bytes, **including** the 16-byte trailer |
| 5      | 4    | `crc32` — CRC32 of the whole image including the trailer (see below) |

The `crc32` field is a transfer integrity check only. The authority on
whether the file is a valid firmware image is the trailer, checked by the
device during VERIFY.

Response:

| Offset | Size | Field                                          |
| ------ | ---- | ---------------------------------------------- |
| 0      | 1    | Command ID (18)                                |
| 1      | 1    | Status                                         |
| 2      | 1    | `chunk_size` — max data bytes per WRITE (56)   |
| 3      | 2    | `firmware_version` — currently running version |
| 5      | 4    | `staging_size` — staging capacity in bytes     |
| 9      | 4    | `app_max_size` — application region size       |

### COMMAND_FW_UPDATE_WRITE (19)

Request:

| Offset | Size | Field                                             |
| ------ | ---- | ------------------------------------------------- |
| 0      | 1    | Command ID (19)                                   |
| 1      | 4    | `offset` — byte offset of this chunk in the image |
| 5      | 1    | `len` — chunk length (1..56)                      |
| 6      | 56   | `data` — chunk data (only first `len` bytes used) |

Chunks must be sent strictly sequentially starting at offset 0. Every chunk
except the final one must have a length that is a multiple of 4. The final
partial word (if any) is padded with `0xFF` before programming.

The firmware erases each 2KB staging sector immediately before the first
write that touches it, so a WRITE that crosses a sector boundary can take a
sector-erase longer than usual (tens of milliseconds, worst case up to a few
hundred). Hosts should use a request timeout of at least 1 second.

Response:

| Offset | Size | Field                                             |
| ------ | ---- | ------------------------------------------------- |
| 0      | 1    | Command ID (19)                                   |
| 1      | 1    | Status                                            |
| 2      | 4    | `next_offset` — next expected sequential offset   |

### COMMAND_FW_UPDATE_VERIFY (20)

Request: command ID only.

The firmware verifies the staged image in this order:

1. **Transfer integrity** — CRC32 over the whole staged image (payload +
   trailer, single shot) must match the value given at INIT. Failure:
   `ERR_CRC` (7).
2. **Image self-verification** — the last 16 bytes are parsed as the
   trailer:
   - `magic` must be `0x52424649` ("IFBR"); failure: `ERR_IMAGE` (11);
   - `payload_len` must equal `size - 16`; failure: `ERR_IMAGE` (11);
   - CRC32 computed over the payload (first `size - 16` bytes) must match
     `payload_crc32` from the trailer; failure: `ERR_IMAGE` (11). This is
     the authoritative validity check: the expected value was embedded at
     build time, not supplied by the host.
   - `version` is read but not enforced (downgrades allowed).
3. **Vector table** of the payload:
   - word 0 (initial stack pointer) must lie within `0x20000000 - 0x20019800`
     (102KB RAM);
   - word 1 (reset vector) must be a Thumb address (bit 0 set) inside the
     application region `0x08000000 - 0x08017000`.

   Failure: `ERR_VECTOR` (8).

Response:

| Offset | Size | Field                                                    |
| ------ | ---- | -------------------------------------------------------- |
| 0      | 1    | Command ID (20)                                          |
| 1      | 1    | Status                                                   |
| 2      | 4    | `crc32` — CRC32 computed over the whole staged image     |

### COMMAND_FW_UPDATE_APPLY (21)

Request:

| Offset | Size | Field                                        |
| ------ | ---- | -------------------------------------------- |
| 0      | 1    | Command ID (21)                              |
| 1      | 4    | `magic` — must be `0x594C5041` ("APLY" LE)   |

Response:

| Offset | Size | Field           |
| ------ | ---- | --------------- |
| 0      | 1    | Command ID (21) |
| 1      | 1    | Status          |

On `IAP_STATUS_OK` the firmware waits ~250ms (so this response reaches the
host), then disables all interrupts and runs the RAM-resident routine: erase
application region, copy the staged **payload** (image minus the 16-byte
trailer) over it, verify word by word (up to 3 attempts), system reset. The
trailer stays behind in the staging area and is never written to the
application region. (Word-granular copying may carry up to 3 trailer bytes
past a non-word-aligned payload end; these land beyond the application
image and are inert.) The device drops off the bus and re-enumerates with
the new firmware. The host should treat the disconnect as expected and wait
for re-enumeration (suggested timeout: 30 seconds).

If all attempts fail the device still resets; if the application is corrupt,
the ROM DFU bootloader (hold BOOT while plugging in) remains available as the
recovery path.

## CRC32 definition

The firmware uses the AT32F405 hardware CRC unit via
`crc32_compute(buf, len, crc)` with `crc = 0`. The unit is configured
explicitly in `crc32_init` (values equal the peripheral reset defaults):

- polynomial `0x04C11DB7`, 32-bit, MSB-first (no input/output bit reversal)
- data register reset value `0xFFFFFFFF`

`crc32_compute(buf, len, 0)` is therefore equivalent to:

1. `reg = 0xFFFFFFFF`
2. Feed the word `0x00000000` (the `crc` argument).
3. Feed each 32-bit little-endian word of the buffer.
4. If `len % 4 != 0`, feed the remaining bytes zero-padded to a word
   (little-endian).
5. The result is `reg` (no final XOR, no reflection).

Feeding a word means: `reg ^= word`, then 32 iterations of
`reg = (reg & 0x80000000) ? (reg << 1) ^ 0x04C11DB7 : reg << 1`.

Note this is **not** the zlib/PNG CRC32 (which is reflected with final XOR).

Test vectors (bytes -> CRC32):

| Input (bytes)                 | CRC32        |
| ----------------------------- | ------------ |
| `"1234"` (31 32 33 34)        | `0x6C09720A` |
| `"ReLow60"`                   | `0xB10C2C1B` |
| `00 00 00 00`                 | `0x6904BB59` |
| `00 01 02 ... 0F` (16 bytes)  | `0x1310300D` |
| `FF` (single byte)            | `0xD8F3FBED` |

## Typical sequence

```
Host                                   Keyboard
----                                   --------
INIT(size, crc32)                 -->
                                  <--  OK, chunk_size=56, version, capacities
WRITE(0, 56, data)                -->
                                  <--  OK, next=56
WRITE(56, 56, data)               -->
                                  <--  OK, next=112
...                                    (sector erases interleaved on demand)
WRITE(n, last_len, data)          -->
                                  <--  OK, next=size
VERIFY()                          -->
                                  <--  OK, crc32
APPLY(0x594C5041)                 -->
                                  <--  OK
                                       (~250ms later: USB drops,
                                        1-3s flash rewrite, reset)
        ... host waits for re-enumeration, reconnects via WebHID ...
FIRMWARE_VERSION                  -->
                                  <--  new version
```

## Compatibility notes

- Older firmware (<= v1.12 without this feature) does not recognize command
  IDs 18-21 and replies with `COMMAND_UNKNOWN` (255). Since the host matches
  responses by echoed command ID, such a request times out on the host side;
  hosts should treat a timeout on INIT as "IAP not supported" and fall back
  to the DFU path.
- Only `firmware_iap.bin` (with trailer) is accepted by the IAP path. A
  plain `firmware.bin` — with or without its DFU suffix — has no trailer
  and fails VERIFY with `ERR_IMAGE`. Hosts should additionally check for
  the trailer magic before starting a transfer and tell the user to pick
  `firmware_iap.bin`.
- The linker now caps the application at 92KB (previously 184KB). The current
  application is ~42KB. Since the staging area is also 92KB and the IAP image
  carries a 16-byte trailer, the effective IAP payload cap is 92KB - 16
  bytes. If the application ever outgrows that, the partition scheme (and
  this protocol's capacities) must be revised.
- The WebUSB DFU update path is unchanged (it uses the trailer-less
  `firmware.bin`) and remains the recovery mechanism.
