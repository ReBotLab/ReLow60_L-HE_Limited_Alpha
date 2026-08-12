# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.

# Post-build step: generate `firmware_iap.bin`, the image used by the in-app
# firmware update (IAP) path, next to the regular `firmware.bin`.
#
# `firmware_iap.bin` is the raw application binary followed by a 16-byte
# self-verification trailer (little-endian):
#
#   | Offset | Size | Field                                            |
#   | ------ | ---- | ------------------------------------------------ |
#   | 0      | 4    | magic = 0x52424649 ("IFBR")                      |
#   | 4      | 2    | version = FIRMWARE_VERSION (include/common.h)    |
#   | 6      | 2    | reserved (0)                                     |
#   | 8      | 4    | payload_len = payload size in bytes              |
#   | 12     | 4    | payload_crc32 = AT32 CRC32 of the payload        |
#
# The device recomputes the payload CRC32 during VERIFY and compares it with
# the value embedded here at build time, so a file corrupted at rest (not
# just in transfer) is rejected. See `docs/iap-protocol.md` and
# `include/iap.h`.
#
# Ordering note: the arterytekat32 platform builder registers a post action
# on `${PROGNAME}.bin` that appends a 16-byte DFU suffix in place ("Suffix
# successfully added" in the build log). This script's post action is
# registered later (post extra_scripts run after the platform builder), so
# by the time it runs, `firmware.bin` already carries that suffix. The DFU
# suffix is stripped before the trailer is computed: the IAP trailer must
# vouch for the pure application payload, and `firmware_iap.bin` is not a
# DFU file. `firmware.bin` itself (DFU flashing path) is left untouched.

import re
import struct

Import("env")  # type: ignore[name-defined]  # noqa: F821

IAP_TRAILER_MAGIC = 0x52424649  # "IFBR" little-endian
IAP_TRAILER_SIZE = 16

CRC32_POLY = 0x04C11DB7


def crc32_at32_word(reg: int, word: int) -> int:
    """Feed one 32-bit word into the CRC register (MSB first, no reflection)."""
    reg ^= word
    for _ in range(32):
        if reg & 0x80000000:
            reg = ((reg << 1) ^ CRC32_POLY) & 0xFFFFFFFF
        else:
            reg = (reg << 1) & 0xFFFFFFFF
    return reg


def crc32_at32(data: bytes, initial: int = 0) -> int:
    """AT32F405 hardware CRC32 as used by `crc32_compute(buf, len, initial)`.

    Mirrors `src/hardware/at32f405xx/crc32.c`:
      1. reg = 0xFFFFFFFF (crc_data_reset)
      2. feed the `initial` argument as one word (always 0 for IAP)
      3. feed the buffer as little-endian 32-bit words
      4. feed the final partial word zero-padded (little-endian)
      5. no output reversal, no final XOR
    """
    reg = 0xFFFFFFFF
    reg = crc32_at32_word(reg, initial & 0xFFFFFFFF)

    full = len(data) & ~3
    for off in range(0, full, 4):
        reg = crc32_at32_word(reg, struct.unpack_from("<I", data, off)[0])

    rem = len(data) - full
    if rem:
        reg = crc32_at32_word(
            reg, struct.unpack("<I", data[full:] + b"\x00" * (4 - rem))[0]
        )

    return reg


def crc32_self_test() -> None:
    """Verify the Python reimplementation against the protocol test vectors.

    Vectors are from `docs/iap-protocol.md` (repository root) and match the
    hardware CRC unit and the ReConf TypeScript implementation.
    """
    vectors = [
        (b"1234", 0x6C09720A),
        (b"ReLow60", 0xB10C2C1B),
        (b"\x00\x00\x00\x00", 0x6904BB59),
        (bytes(range(16)), 0x1310300D),
        (b"\xff", 0xD8F3FBED),
    ]
    for data, expected in vectors:
        actual = crc32_at32(data)
        if actual != expected:
            raise AssertionError(
                f"AT32 CRC32 self-test failed for {data!r}: "
                f"expected 0x{expected:08X}, got 0x{actual:08X}"
            )


def strip_dfu_suffix(data: bytes) -> bytes:
    """Return `data` without its trailing 16-byte DFU suffix, if present.

    A DFU 1.1 suffix ends with: ucDfuSignature = "UFD" at [-8:-5], bLength
    (16) at [-5], dwCRC at [-4:]. The platform builder appends one to
    `firmware.bin` for dfu-util; it must not be part of the IAP payload.
    """
    if len(data) >= IAP_TRAILER_SIZE and data[-8:-5] == b"UFD" and data[-5] == 16:
        return data[:-16]
    return data


def read_firmware_version(env) -> int:
    """Parse FIRMWARE_VERSION out of `include/common.h`."""
    common_h = env.subst("$PROJECT_INCLUDE_DIR/common.h")
    with open(common_h, "r", encoding="utf-8") as f:
        match = re.search(
            r"^\s*#define\s+FIRMWARE_VERSION\s+(0[xX][0-9a-fA-F]+|\d+)",
            f.read(),
            re.MULTILINE,
        )
    if match is None:
        raise AssertionError(f"FIRMWARE_VERSION not found in {common_h}")
    return int(match.group(1), 0)


def build_iap_image(source, target, env) -> None:
    crc32_self_test()

    bin_path = target[0].get_abspath()
    iap_path = re.sub(r"\.bin$", "", bin_path) + "_iap.bin"

    with open(bin_path, "rb") as f:
        data = f.read()

    payload = strip_dfu_suffix(data)
    if len(payload) != len(data):
        print(f"iap_image: stripped 16-byte DFU suffix from {bin_path}")

    version = read_firmware_version(env)
    payload_crc32 = crc32_at32(payload)

    trailer = struct.pack(
        "<IHHII",
        IAP_TRAILER_MAGIC,
        version,
        0,  # reserved
        len(payload),
        payload_crc32,
    )
    assert len(trailer) == IAP_TRAILER_SIZE

    with open(iap_path, "wb") as f:
        f.write(payload + trailer)

    print(
        f"iap_image: wrote {iap_path} "
        f"(payload {len(payload)} bytes, version 0x{version:04X}, "
        f"payload CRC32 0x{payload_crc32:08X})"
    )


env.AddPostAction(
    "$BUILD_DIR/${PROGNAME}.bin",
    env.VerboseAction(
        build_iap_image, "Generating ${PROGNAME}_iap.bin (IAP trailer)"
    ),
)
