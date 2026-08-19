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

#include "advanced_keys.h"
#include "commands.h"
#include "crc32.h"
#include "deferred_actions.h"
#include "eeconfig.h"
#include "hardware/hardware.h"
#include "hid.h"
#include "iap.h"
#include "layout.h"
#include "macro.h"
#include "matrix.h"
#include "measurement.h"
#include "polling_test.h"
#ifdef OLED_ENABLED
#include "oled_display.h"
#endif
#include "tusb.h"
#include "wear_leveling.h"
#include "xinput.h"

int main(void) {
  // Initialize the hardware
  board_init();
  timer_init();
  crc32_init();
  flash_init();

  // Initialize the persistent configuration
  wear_leveling_init();
  eeconfig_init();

  // Hold BOOT button after power-on to factory reset
  if (board_factory_reset_requested()) {
    eeconfig_reset();
    board_reset();
  }

  // Initialize the core modules
  analog_init();
  matrix_init();
  hid_init();
  deferred_action_init();
  macro_init();
  advanced_key_init();
  xinput_init();
  layout_init();
  command_init();
  polling_test_init();

  tud_init(BOARD_TUD_RHPORT);

#ifdef OLED_ENABLED
  oled_display_init();
#endif

  while (1) {
    tud_task();

    if (polling_test_task())
      // The USB polling self-test owns the loop. Skipping the scan tasks keeps
      // the firmware from being the bottleneck in its own measurement.
      continue;

    measurement_task();

    analog_task();
    matrix_scan();
    layout_task();
    xinput_task();

    // Performs a pending firmware update apply (does not return in that case)
    iap_task();

#ifdef OLED_ENABLED
    oled_display_task();
#endif
  }

  return 0;
}
