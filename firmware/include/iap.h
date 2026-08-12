/*
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "common.h"

//--------------------------------------------------------------------+
// In-Application Programming (IAP) Configuration
//
// The internal flash is partitioned as follows (see also the protocol
// documentation in `docs/iap-protocol.md` at the repository root):
//
//   0x08000000 - 0x08017000  Application (92KB)
//   0x08017000 - 0x0802E000  IAP staging area (92KB)
//   0x0802E000 - 0x08040000  Wear leveling backing store (72KB)
//
// A new firmware image is streamed into the staging area over raw HID,
// verified, and then copied over the application region by a RAM-resident
// routine followed by a system reset.
//--------------------------------------------------------------------+

// Base address of the internal flash
#define IAP_FLASH_BASE_ADDR 0x08000000UL

// Maximum size of an application image in bytes. This must match
// `APP_MAX_SIZE` in the linker script, which caps the application at link
// time.
#define IAP_APP_MAX_SIZE 0x17000UL

// Staging area for incoming images, expressed as an offset from the flash
// base. The staging area must never overlap the wear leveling area starting
// at `WL_BASE_ADDRESS`.
#define IAP_STAGING_OFFSET IAP_APP_MAX_SIZE
#define IAP_STAGING_SIZE IAP_APP_MAX_SIZE
#define IAP_STAGING_ADDR (IAP_FLASH_BASE_ADDR + IAP_STAGING_OFFSET)

#if defined(WL_BASE_ADDRESS)
_Static_assert(IAP_STAGING_OFFSET + IAP_STAGING_SIZE <= WL_BASE_ADDRESS,
               "IAP staging area overlaps the wear leveling area");
#endif

// RAM range used to validate the initial stack pointer of an incoming image
#define IAP_RAM_BASE 0x20000000UL
#define IAP_RAM_SIZE (102UL * 1024UL)

// Minimum plausible payload (application) size, excluding the trailer. The
// payload must at least contain a vector table.
#define IAP_MIN_IMAGE_SIZE 512UL

//--------------------------------------------------------------------+
// Image Self-Verification Trailer
//
// An IAP image (`firmware_iap.bin`, produced by `scripts/iap_image.py`) is
// the raw application binary followed by a 16-byte trailer:
//
//   [payload (application binary)][trailer (16 bytes)]
//
// Trailer layout (little-endian):
//
//   | Offset | Size | Field                                       |
//   | ------ | ---- | ------------------------------------------- |
//   | 0      | 4    | magic = IAP_TRAILER_MAGIC ("IFBR")          |
//   | 4      | 2    | version = FIRMWARE_VERSION of the payload   |
//   | 6      | 2    | reserved (0)                                |
//   | 8      | 4    | payload_len = payload size in bytes         |
//   | 12     | 4    | payload_crc32 = CRC32 of the payload        |
//
// The trailer makes the image self-describing: VERIFY checks the payload
// against the CRC32 *embedded in the image at build time*, so a file that
// was corrupted before the host ever saw it is rejected. A host-supplied
// CRC (INIT) cannot provide this guarantee, since the host computes it over
// the same possibly-corrupted file.
//--------------------------------------------------------------------+

// Trailer magic ("IFBR" as a little-endian string: Iap Firmware Blob
// tRailer)
#define IAP_TRAILER_MAGIC 0x52424649UL

// Total trailer size in bytes
#define IAP_TRAILER_SIZE 16UL

// Trailer as stored at the end of the staged image. All fields are
// little-endian; read it with `memcpy` since the payload length (and thus
// the trailer offset) is not guaranteed to be word aligned.
typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t payload_len;
  uint32_t payload_crc32;
} iap_trailer_t;

_Static_assert(sizeof(iap_trailer_t) == IAP_TRAILER_SIZE,
               "IAP trailer struct must be exactly IAP_TRAILER_SIZE bytes");

// Data bytes per `COMMAND_FW_UPDATE_WRITE` report. Must be a multiple of 4
// and fit in a 64-byte report alongside the 6-byte header.
#define IAP_CHUNK_SIZE 56

// Magic value required by `COMMAND_FW_UPDATE_APPLY` ("APLY" as a
// little-endian string)
#define IAP_APPLY_MAGIC 0x594C5041UL

// Delay in milliseconds between acknowledging APPLY and starting the actual
// flash operation, giving the host time to receive the response before USB
// stops responding.
#define IAP_APPLY_DELAY_MS 250

//--------------------------------------------------------------------+
// IAP Status Codes
//--------------------------------------------------------------------+

typedef enum {
  IAP_STATUS_OK = 0,
  // Command received in an incompatible state
  IAP_STATUS_ERR_STATE,
  // Image size is zero, too small, or exceeds the staging capacity
  IAP_STATUS_ERR_SIZE,
  // WRITE offset does not match the expected sequential offset
  IAP_STATUS_ERR_OFFSET,
  // WRITE length is invalid (zero, too large, past the end of the image, or
  // not a multiple of 4 for a non-final chunk)
  IAP_STATUS_ERR_LENGTH,
  // Flash erase failed
  IAP_STATUS_ERR_ERASE,
  // Flash write failed
  IAP_STATUS_ERR_WRITE,
  // CRC32 of the staged image does not match the value given at INIT
  // (transfer between host and device was corrupted)
  IAP_STATUS_ERR_CRC,
  // Vector table of the staged image is invalid
  IAP_STATUS_ERR_VECTOR,
  // APPLY magic value mismatch
  IAP_STATUS_ERR_MAGIC,
  // Flash geometry does not permit IAP (staging base not sector aligned)
  IAP_STATUS_ERR_GEOMETRY,
  // Image self-verification failed: trailer magic missing, trailer payload
  // length inconsistent with the image size, or payload CRC32 does not match
  // the value embedded in the trailer (the file is not a valid IAP image or
  // was corrupted before the transfer)
  IAP_STATUS_ERR_IMAGE,
} iap_status_t;

//--------------------------------------------------------------------+
// IAP API
//--------------------------------------------------------------------+

/**
 * @brief Begin a firmware update transfer
 *
 * Resets the transfer state machine. Can be called at any time except while
 * an apply operation is pending.
 *
 * @param image_size Total size of the incoming image in bytes, including the
 *                    16-byte self-verification trailer
 * @param image_crc32 CRC32 of the whole image including the trailer (see
 *                    `crc32_compute` with an initial value of 0). Used as a
 *                    transfer integrity check only; image validity is
 *                    established by the trailer during VERIFY.
 *
 * @return IAP status code
 */
uint8_t iap_init(uint32_t image_size, uint32_t image_crc32);

/**
 * @brief Write a chunk of the incoming image to the staging area
 *
 * Chunks must be written sequentially starting at offset 0. Staging sectors
 * are erased on demand right before they are first written.
 *
 * @param offset Byte offset of the chunk within the image
 * @param data Chunk data
 * @param len Chunk length in bytes (at most `IAP_CHUNK_SIZE`; must be a
 *            multiple of 4 except for the final chunk)
 * @param next_offset Set to the next expected offset
 *
 * @return IAP status code
 */
uint8_t iap_write(uint32_t offset, const uint8_t *data, uint32_t len,
                  uint32_t *next_offset);

/**
 * @brief Verify the staged image
 *
 * Performs, in order:
 *
 * 1. Transfer integrity: CRC32 over the whole staged image (payload +
 *    trailer) must match the value given at INIT (`IAP_STATUS_ERR_CRC`).
 * 2. Image self-verification: the trailer magic must be present, the
 *    trailer payload length must equal `image_size - IAP_TRAILER_SIZE`, and
 *    the CRC32 of the payload must match the CRC32 embedded in the trailer
 *    at build time (`IAP_STATUS_ERR_IMAGE`). This is the authoritative
 *    validity check; it rejects files that were corrupted before the host
 *    ever read them.
 * 3. Vector table validation of the payload (initial stack pointer and
 *    reset vector, `IAP_STATUS_ERR_VECTOR`).
 *
 * The trailer version field is informational only; downgrades are allowed.
 *
 * @param computed_crc32 Set to the computed CRC32 of the whole staged image
 *
 * @return IAP status code
 */
uint8_t iap_verify(uint32_t *computed_crc32);

/**
 * @brief Request applying the staged image
 *
 * The image must have been verified first. The actual flash operation is
 * performed from `iap_task` after `IAP_APPLY_DELAY_MS` so that the command
 * response can reach the host beforehand. The device then resets; it does not
 * respond over USB while the update is applied.
 *
 * @param magic Must be `IAP_APPLY_MAGIC`
 *
 * @return IAP status code
 */
uint8_t iap_apply_request(uint32_t magic);

/**
 * @brief IAP housekeeping task, called from the main loop
 *
 * Performs a pending apply operation. Does not return if an apply is due
 * (the device resets).
 *
 * @return None
 */
void iap_task(void);

/**
 * @brief Hardware-specific apply routine
 *
 * Disables interrupts, then erases the application region and copies the
 * staged image over it from a RAM-resident routine, and finally resets the
 * system. Does not return on success.
 *
 * A weak default implementation is provided that returns false, for hardware
 * without IAP support.
 *
 * @param image_size Number of bytes to copy from the staging area. The
 *                   caller passes the payload size only, so the trailer is
 *                   never written to the application region (it remains in
 *                   the staging area).
 *
 * @return false if in-place apply is not supported on this hardware
 */
bool iap_hw_apply(uint32_t image_size);
