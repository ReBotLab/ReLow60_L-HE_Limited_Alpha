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
// Board-Specific API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the board
 *
 * This function should initialize the clock, GPIO, and any other peripherals
 * needed by the board. It will be called at the beginning of the program.
 *
 * @return None
 */
void board_init(void);

/**
 * @brief Error handler
 *
 * This function should be called when an unrecoverable error occurs. It should
 * not return.
 *
 * @return None
 */
void board_error_handler(void);

/**
 * @brief Reset the board
 *
 * This function should not return.
 *
 * @return None
 */
void board_reset(void);

/**
 * @brief Enter the bootloader mode
 *
 * If bootloader mode is supported, this function should not return.
 *
 * @return None
 */
void board_enter_bootloader(void);

/**
 * @brief Get the board serial number
 *
 * The serial number is a UTF-8 string with a maximum length of 32 and no null
 * terminator.
 *
 * @param buf Buffer to store the serial number
 *
 * @return Length of the serial number in characters
 */
uint32_t board_serial(char *buf);

/**
 * @brief Check if factory reset is requested via hardware (e.g. BOOT button)
 *
 * Called once during startup. Returns true if the user is holding the BOOT
 * button, signaling that a factory reset should be performed.
 *
 * @return true if factory reset is requested
 */
bool board_factory_reset_requested(void);

/**
 * @brief Get the current cycle count
 *
 * This function returns the current cycle count of the board's CPU. It can be
 * used for performance measurement or timing purposes.
 *
 * @return Current cycle count
 */
uint32_t board_cycle_count(void);

/**
 * @brief Get the current USB frame number
 *
 * This function returns the frame number counter maintained in hardware by the
 * USB peripheral. The counter is incremented on every SOF packet received from
 * the host, which is once per 125us microframe on USB HS and once per 1ms frame
 * on USB FS. It is therefore a jitter-free time base for measuring how well the
 * host keeps up with the endpoint polling interval.
 *
 * The counter is 14 bits wide and wraps around, so callers must mask the
 * difference between two samples with `USB_FRAME_NUMBER_MASK`.
 *
 * @return Current USB frame number
 */
uint32_t board_usb_frame_number(void);

// The USB frame number counter is 14 bits wide
#define USB_FRAME_NUMBER_MASK 0x3FFF
