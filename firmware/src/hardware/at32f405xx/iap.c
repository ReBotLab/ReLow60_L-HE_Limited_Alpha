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

#include "at32f402_405.h"

// The AT32F405 has a single flash bank, so instruction fetch from flash
// stalls while the flash controller erases or programs. The apply routine
// below therefore runs entirely from RAM: it is placed in `.ramfunc`, which
// the linker script locates inside `.data` so the startup code copies it into
// RAM. It must not call any flash-resident function (including the at32
// library), since the application region it executes over is being erased.

// FLASH->sts error bits: prgmerr (bit 2) | epperr (bit 4)
#define IAP_FLASH_STS_ERR_MASK 0x14UL

// Generous busy-wait bound for a single flash operation. A 2KB sector erase
// takes tens of milliseconds; at 216MHz this bound is on the order of
// seconds, so it only guards against a wedged flash controller.
#define IAP_FLASH_OP_TIMEOUT 0x08000000UL

// Number of erase + program + verify attempts before giving up
#define IAP_APPLY_MAX_ATTEMPTS 3UL

/**
 * @brief Wait until the flash controller is idle
 *
 * Always inlined so the code lives inside the RAM-resident caller.
 *
 * @return true if the controller became idle, false on timeout
 */
__attribute__((always_inline)) static inline bool iap_ram_wait_idle(void) {
  for (uint32_t t = 0; t < IAP_FLASH_OP_TIMEOUT; t++) {
    if (!FLASH->sts_bit.obf)
      return true;
  }
  return false;
}

/**
 * @brief Erase the application region, copy the staged image over it, verify
 * the copy, and reset the system
 *
 * Runs from RAM with all interrupts disabled. Touches only flash controller
 * and SCB registers directly; never calls into flash-resident code. Does not
 * return.
 *
 * @param num_words Image size in 32-bit words (rounded up)
 *
 * @return None
 */
__attribute__((noinline, used, section(".ramfunc"))) static void
iap_apply_ram(uint32_t num_words) {
  volatile uint32_t *app = (volatile uint32_t *)IAP_FLASH_BASE_ADDR;
  const volatile uint32_t *staging = (const volatile uint32_t *)IAP_STAGING_ADDR;

  for (uint32_t attempt = 0; attempt < IAP_APPLY_MAX_ATTEMPTS; attempt++) {
    bool ok = true;

    // Unlock the flash controller
    if (FLASH->ctrl_bit.oplk) {
      FLASH->unlock = FLASH_UNLOCK_KEY1;
      FLASH->unlock = FLASH_UNLOCK_KEY2;
    }

    if (!iap_ram_wait_idle())
      ok = false;

    // Clear stale error flags (write 1 to clear)
    FLASH->sts = IAP_FLASH_STS_ERR_MASK;

    // Erase the whole application region, one sector at a time
    for (uint32_t addr = IAP_FLASH_BASE_ADDR;
         ok && addr < IAP_FLASH_BASE_ADDR + IAP_APP_MAX_SIZE;
         addr += FLASH_SECTOR_SIZE) {
      FLASH->ctrl_bit.secers = TRUE;
      FLASH->addr = addr;
      FLASH->ctrl_bit.erstr = TRUE;

      if (!iap_ram_wait_idle())
        ok = false;
      FLASH->ctrl_bit.secers = FALSE;

      if (FLASH->sts & IAP_FLASH_STS_ERR_MASK) {
        FLASH->sts = IAP_FLASH_STS_ERR_MASK;
        ok = false;
      }
    }

    // Program the staged image into the application region
    for (uint32_t i = 0; ok && i < num_words; i++) {
      const uint32_t word = staging[i];

      FLASH->ctrl_bit.fprgm = TRUE;
      app[i] = word;

      if (!iap_ram_wait_idle())
        ok = false;
      FLASH->ctrl_bit.fprgm = FALSE;

      if (FLASH->sts & IAP_FLASH_STS_ERR_MASK) {
        FLASH->sts = IAP_FLASH_STS_ERR_MASK;
        ok = false;
      }
    }

    // Verify the copy word by word
    for (uint32_t i = 0; ok && i < num_words; i++) {
      if (app[i] != staging[i])
        ok = false;
    }

    if (ok)
      break;
  }

  // Lock the flash controller again
  FLASH->ctrl_bit.oplk = TRUE;

  // Reset regardless of the outcome. If all attempts failed, the application
  // may be corrupt; the ROM DFU bootloader (BOOT button) remains available as
  // a recovery path.
  __DSB();
  SCB->AIRCR = (0x5FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
  __DSB();
  while (1)
    ;
}

bool iap_hw_apply(uint32_t image_size) {
  // Stop SysTick and disable + clear all interrupts. USB stops responding
  // here; the host is expected to wait for the device to re-enumerate.
  __disable_irq();

  SysTick->CTRL = 0;
  SysTick->LOAD = 0;
  SysTick->VAL = 0;

  for (uint32_t i = 0; i < M_ARRAY_SIZE(NVIC->ICER); i++) {
    NVIC->ICER[i] = 0xFFFFFFFFUL;
    NVIC->ICPR[i] = 0xFFFFFFFFUL;
  }

  __DSB();
  __ISB();

  iap_apply_ram((image_size + 3) / 4);

  // Unreachable: iap_apply_ram always resets the system
  return false;
}
