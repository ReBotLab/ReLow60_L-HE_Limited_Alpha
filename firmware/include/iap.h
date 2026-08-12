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

// Minimum plausible image size (must at least contain a vector table)
#define IAP_MIN_IMAGE_SIZE 512UL

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
  IAP_STATUS_ERR_CRC,
  // Vector table of the staged image is invalid
  IAP_STATUS_ERR_VECTOR,
  // APPLY magic value mismatch
  IAP_STATUS_ERR_MAGIC,
  // Flash geometry does not permit IAP (staging base not sector aligned)
  IAP_STATUS_ERR_GEOMETRY,
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
 * @param image_size Size of the incoming image in bytes
 * @param image_crc32 CRC32 of the image (see `crc32_compute` with an initial
 *                    value of 0)
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
 * Computes the CRC32 of the staged image and compares it against the value
 * given at INIT, then validates the vector table (initial stack pointer and
 * reset vector).
 *
 * @param computed_crc32 Set to the computed CRC32 of the staged image
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
 * @param image_size Size of the staged image in bytes
 *
 * @return false if in-place apply is not supported on this hardware
 */
bool iap_hw_apply(uint32_t image_size);
