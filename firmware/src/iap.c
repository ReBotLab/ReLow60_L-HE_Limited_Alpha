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

#include "iap.h"

#include "crc32.h"
#include "hardware/hardware.h"

//--------------------------------------------------------------------+
// Transfer State Machine
//--------------------------------------------------------------------+

typedef enum {
  // No transfer in progress
  IAP_STATE_IDLE = 0,
  // INIT accepted, receiving chunks
  IAP_STATE_RECEIVING,
  // All bytes received, awaiting VERIFY
  IAP_STATE_RECEIVED,
  // VERIFY passed, awaiting APPLY
  IAP_STATE_VERIFIED,
  // APPLY accepted, waiting for the response to flush before applying
  IAP_STATE_APPLYING,
} iap_state_t;

static iap_state_t iap_state = IAP_STATE_IDLE;
static uint32_t iap_image_size;
static uint32_t iap_expected_crc32;
static uint32_t iap_next_offset;

// Erase-on-demand bookkeeping. Erasing the whole staging area at INIT would
// stall instruction fetch (single-bank flash) for far too long, so staging
// sectors are erased one at a time right before they are first written.
static uint32_t iap_erased_bytes;
static uint32_t iap_next_erase_sector;

// Pending apply bookkeeping
static bool iap_apply_pending;
static uint32_t iap_apply_time;

/**
 * @brief Find the flash sector whose start offset is `IAP_STAGING_OFFSET`
 *
 * @param sector Set to the sector index on success
 *
 * @return true if the staging base is sector aligned, false otherwise
 */
static bool iap_staging_start_sector(uint32_t *sector) {
  uint32_t offset = 0;

  for (uint32_t s = 0; offset < IAP_STAGING_OFFSET; s++) {
    const uint32_t size = flash_sector_size(s);

    if (size == 0)
      // Ran past the end of the flash
      return false;

    offset += size;
    if (offset == IAP_STAGING_OFFSET) {
      *sector = s + 1;
      return true;
    }
  }

  return false;
}

uint8_t iap_init(uint32_t image_size, uint32_t image_crc32) {
  if (iap_state == IAP_STATE_APPLYING)
    return IAP_STATUS_ERR_STATE;

  if (image_size < IAP_MIN_IMAGE_SIZE || image_size > IAP_STAGING_SIZE)
    return IAP_STATUS_ERR_SIZE;

  uint32_t start_sector = 0;
  if (!iap_staging_start_sector(&start_sector))
    return IAP_STATUS_ERR_GEOMETRY;

  iap_image_size = image_size;
  iap_expected_crc32 = image_crc32;
  iap_next_offset = 0;
  iap_erased_bytes = 0;
  iap_next_erase_sector = start_sector;
  iap_apply_pending = false;
  iap_state = IAP_STATE_RECEIVING;

  return IAP_STATUS_OK;
}

uint8_t iap_write(uint32_t offset, const uint8_t *data, uint32_t len,
                  uint32_t *next_offset) {
  *next_offset = iap_next_offset;

  if (iap_state != IAP_STATE_RECEIVING)
    return IAP_STATUS_ERR_STATE;

  if (offset != iap_next_offset)
    return IAP_STATUS_ERR_OFFSET;

  if (len == 0 || len > IAP_CHUNK_SIZE || offset + len > iap_image_size)
    return IAP_STATUS_ERR_LENGTH;

  const bool final_chunk = (offset + len == iap_image_size);
  if ((len & 3) != 0 && !final_chunk)
    // Only the final chunk may have a length that is not a multiple of 4
    return IAP_STATUS_ERR_LENGTH;

  // Erase staging sectors on demand before they are first written
  while (iap_erased_bytes < offset + len) {
    const uint32_t sector_size = flash_sector_size(iap_next_erase_sector);

    if (sector_size == 0 || !flash_erase(iap_next_erase_sector)) {
      iap_state = IAP_STATE_IDLE;
      return IAP_STATUS_ERR_ERASE;
    }
    iap_erased_bytes += sector_size;
    iap_next_erase_sector++;
  }

  // Copy the chunk into a word-aligned buffer, padding the final partial word
  // with the flash empty value
  uint32_t words[IAP_CHUNK_SIZE / 4];
  memset(words, 0xFF, sizeof(words));
  memcpy(words, data, len);

  if (!flash_write(IAP_STAGING_OFFSET + offset, words, (len + 3) / 4)) {
    iap_state = IAP_STATE_IDLE;
    return IAP_STATUS_ERR_WRITE;
  }

  iap_next_offset = offset + len;
  *next_offset = iap_next_offset;
  if (iap_next_offset == iap_image_size)
    iap_state = IAP_STATE_RECEIVED;

  return IAP_STATUS_OK;
}

uint8_t iap_verify(uint32_t *computed_crc32) {
  *computed_crc32 = 0;

  if (iap_state != IAP_STATE_RECEIVED && iap_state != IAP_STATE_VERIFIED)
    return IAP_STATUS_ERR_STATE;

  // The whole image must be hashed in a single `crc32_compute` call: the
  // hardware implementation resets the CRC unit at the start of each call, so
  // chaining calls is not equivalent to hashing the concatenated input.
  const uint32_t crc =
      crc32_compute((const void *)IAP_STAGING_ADDR, iap_image_size, 0);

  *computed_crc32 = crc;
  if (crc != iap_expected_crc32) {
    iap_state = IAP_STATE_IDLE;
    return IAP_STATUS_ERR_CRC;
  }

  // Validate the vector table of the staged image
  const volatile uint32_t *vectors = (const volatile uint32_t *)IAP_STAGING_ADDR;
  const uint32_t initial_sp = vectors[0];
  const uint32_t reset_vector = vectors[1];

  if (initial_sp < IAP_RAM_BASE || initial_sp > IAP_RAM_BASE + IAP_RAM_SIZE) {
    iap_state = IAP_STATE_IDLE;
    return IAP_STATUS_ERR_VECTOR;
  }

  // The reset vector must be a Thumb address within the application region
  if ((reset_vector & 1) == 0 ||
      (reset_vector & ~1UL) < IAP_FLASH_BASE_ADDR ||
      (reset_vector & ~1UL) >= IAP_FLASH_BASE_ADDR + IAP_APP_MAX_SIZE) {
    iap_state = IAP_STATE_IDLE;
    return IAP_STATUS_ERR_VECTOR;
  }

  iap_state = IAP_STATE_VERIFIED;
  return IAP_STATUS_OK;
}

uint8_t iap_apply_request(uint32_t magic) {
  if (iap_state != IAP_STATE_VERIFIED)
    return IAP_STATUS_ERR_STATE;

  if (magic != IAP_APPLY_MAGIC)
    return IAP_STATUS_ERR_MAGIC;

  iap_state = IAP_STATE_APPLYING;
  iap_apply_pending = true;
  iap_apply_time = timer_read();

  return IAP_STATUS_OK;
}

void iap_task(void) {
  if (!iap_apply_pending)
    return;

  if (timer_elapsed(iap_apply_time) < IAP_APPLY_DELAY_MS)
    return;

  iap_apply_pending = false;
  // Does not return on success (the device resets)
  if (!iap_hw_apply(iap_image_size))
    // In-place apply is not supported on this hardware. Nothing has been
    // modified, so simply drop the transfer.
    iap_state = IAP_STATE_IDLE;
}

__attribute__((weak)) bool iap_hw_apply(uint32_t image_size) {
  (void)image_size;
  return false;
}
