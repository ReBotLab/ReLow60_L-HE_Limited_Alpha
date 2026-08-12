# In-App Firmware Update (IAP) Protocol

This document describes the raw HID protocol used to update the ReLow60 L-HE
firmware from a WebHID host (ReConf) without entering the ROM DFU bootloader.
No WinUSB driver (Zadig) or WebUSB access is required on Windows.

Reference implementation:

- Firmware: `firmware/include/iap.h`, `firmware/src/iap.c`,
  `firmware/src/hardware/at32f405xx/iap.c`
- Host (ReConf): `src/lib/iap/` and `src/lib/libhmk/commands/fw-update.ts`

## Memory map

The AT32F405RCT7 has 256KB of single-bank flash (128 sectors x 2KB).

| Range                     | Size | Use                                  |
| ------------------------- | ---- | ------------------------------------ |
| `0x08000000 - 0x08017000` | 92KB | Application (capped by linker)       |
| `0x08017000 - 0x0802E000` | 92KB | IAP staging area                     |
| `0x0802E000 - 0x08040000` | 72KB | Wear leveling backing store (eeconfig) |

A new image is streamed into the staging area, verified there, and then
copied over the application region by a RAM-resident routine followed by a
system reset. Images larger than 92KB are rejected at INIT.

Because the flash is single-bank, instruction fetch stalls while the flash
controller erases or programs:

- Staging writes are tolerable (sub-millisecond word programming, one 2KB
  sector erase on demand as each sector boundary is crossed).
- Rewriting the application itself must run from RAM with interrupts
  disabled; USB does not respond during this phase (roughly 1-3 seconds) and
  the device then re-enumerates.

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
| 2     | `IAP_STATUS_ERR_SIZE`     | Image size < 512 bytes or > staging capacity       |
| 3     | `IAP_STATUS_ERR_OFFSET`   | WRITE offset != expected sequential offset         |
| 4     | `IAP_STATUS_ERR_LENGTH`   | WRITE length invalid (0, > chunk size, past image end, or non-final chunk not a multiple of 4) |
| 5     | `IAP_STATUS_ERR_ERASE`    | Staging sector erase failed (transfer aborted)     |
| 6     | `IAP_STATUS_ERR_WRITE`    | Staging flash write failed (transfer aborted)      |
| 7     | `IAP_STATUS_ERR_CRC`      | Staged image CRC32 mismatch (transfer aborted)     |
| 8     | `IAP_STATUS_ERR_VECTOR`   | Staged image vector table invalid (transfer aborted) |
| 9     | `IAP_STATUS_ERR_MAGIC`    | APPLY magic mismatch                               |
| 10    | `IAP_STATUS_ERR_GEOMETRY` | Staging base not aligned to a flash sector         |

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
| 1      | 4    | `size` — image size in bytes   |
| 5      | 4    | `crc32` — CRC32 of the image (see below) |

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

The firmware computes the CRC32 over the staged image (single shot) and
compares it with the value given at INIT, then validates the vector table:

- word 0 (initial stack pointer) must lie within `0x20000000 - 0x20019800`
  (102KB RAM);
- word 1 (reset vector) must be a Thumb address (bit 0 set) inside the
  application region `0x08000000 - 0x08017000`.

Response:

| Offset | Size | Field                                      |
| ------ | ---- | ------------------------------------------ |
| 0      | 1    | Command ID (20)                            |
| 1      | 1    | Status                                     |
| 2      | 4    | `crc32` — CRC32 computed over the staging  |

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
application region, copy staging over it, verify word by word (up to 3
attempts), system reset. The device drops off the bus and re-enumerates with
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
- The linker now caps the application at 92KB (previously 184KB). The current
  application is ~42KB. If the application ever outgrows 92KB, the partition
  scheme (and this protocol's capacities) must be revised.
- The WebUSB DFU update path is unchanged and remains the recovery mechanism.
