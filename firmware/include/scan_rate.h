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
// Scan Rate
//--------------------------------------------------------------------+

// How often the key state is evaluated, which together with the USB polling
// rate is what actually decides input latency.
//
// Two different rates matter and they are not interchangeable:
//
//   `matrix_scan()` runs on every main loop iteration without waiting for new
//   samples, so the evaluation rate can exceed the rate at which the analog
//   values themselves change. The iterations above that only re-evaluate data
//   they have already seen, so the real latency floor is the ADC sweep, not the
//   loop. Reporting only the loop rate would flatter the keyboard.
//
// The evaluation rate is measured over a sliding window; the sweep rate is
// fixed by the analog configuration and is computed rather than measured.

// Length of the window the evaluation rate is averaged over
#define SCAN_RATE_WINDOW_MS 250

typedef struct __attribute__((packed)) {
  // Main loop iterations per second, i.e. how often the actuation logic runs.
  // Zero until the first window has elapsed.
  uint32_t eval_rate_hz;
  // Full analog sweeps per second: how often a key's measured value is
  // refreshed. This is the ceiling on how quickly a keypress can be seen.
  uint32_t sweep_rate_hz;
  // Sweep period in microseconds, so the host does not have to divide.
  uint16_t sweep_period_us;
  // Shortest and longest iteration seen in the last window, in microseconds.
  // A large spread means something is stalling the loop.
  uint16_t eval_min_us;
  uint16_t eval_max_us;
} scan_rate_t;

//--------------------------------------------------------------------+
// Scan Rate API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the scan rate module
 *
 * @return None
 */
void scan_rate_init(void);

/**
 * @brief Account for one pass of the scan loop
 *
 * Must be called once per main loop iteration that actually scans the matrix.
 * Iterations skipped by another task (such as the USB polling self-test) must
 * not be counted, or the rate would include loops that scanned nothing.
 *
 * @return None
 */
void scan_rate_task(void);

/**
 * @brief Read the current scan rates
 *
 * @param out Buffer to write the rates to
 *
 * @return None
 */
void scan_rate_get(scan_rate_t *out);
