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
// HID API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the HID module
 *
 * @return None
 */
void hid_init(void);

/**
 * @brief Add a keycode to the appropriate HID report
 *
 * @param keycode Keycode to add
 *
 * @return None
 */
void hid_keycode_add(uint8_t keycode);

/**
 * @brief Remove a keycode from the appropriate HID report
 *
 * @param keycode Keycode to remove
 *
 * @return None
 */
void hid_keycode_remove(uint8_t keycode);

/**
 * @brief Get the modifier byte of the current keyboard report
 *
 * @return Modifier bitmap (bit 0: Left Ctrl ... bit 7: Right GUI)
 */
uint8_t hid_get_modifiers(void);

/**
 * @brief Check whether the keyboard report is empty
 *
 * @return true if no key or modifier is currently reported as pressed
 */
bool hid_keyboard_is_idle(void);

/**
 * @brief Invalidate the cached keyboard report
 *
 * Unchanged keyboard reports are normally suppressed. Call this after sending
 * keyboard reports outside of `hid_send_reports()` to force the next report
 * through, so that the host is resynchronized with the actual key state.
 *
 * @return None
 */
void hid_keyboard_invalidate_cache(void);

/**
 * @brief Send all HID reports
 *
 * This function will block until the device is ready to send the reports.
 *
 * @return None
 */
void hid_send_reports(void);
