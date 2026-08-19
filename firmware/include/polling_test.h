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
// USB Polling Self-Test
//--------------------------------------------------------------------+

// A keyboard only sends a report when the report changes, so an idle keyboard
// produces no bus traffic at all and neither the firmware nor the host can tell
// how often the host actually polls the keyboard endpoint. During a measurement
// window the test keeps the keyboard endpoint continuously armed with an empty
// report, which the host acknowledges on every poll without producing any key
// event, and counts how far apart the acknowledgements are.
//
// Distances are measured with the USB frame number counter maintained by the
// USB peripheral (see `board_usb_frame_number()`) rather than with the CPU
// cycle counter, so the result reflects the bus and not the firmware's own
// scheduling. One tick of that counter is one 125us microframe on high speed
// and one 1ms frame on full speed.
//
// While a window is in progress the main loop skips key scanning entirely, so
// the firmware is never the limiting factor. `loop_count`, `loop_max_cycles`
// and `send_max_cycles` are reported so that this can be verified rather than
// assumed.

// Number of interval histogram buckets
#define POLLING_TEST_NUM_BUCKETS 6
// Bounds for the requested measurement window
#define POLLING_TEST_MIN_WINDOW_MS 100
#define POLLING_TEST_MAX_WINDOW_MS 2000
// Give up if the host stops acknowledging reports for this long
#define POLLING_TEST_STALL_MS 50

// `polling_test_result_t.flags`
#define POLLING_TEST_FLAG_XINPUT_ENABLED (1 << 0)

// `polling_test_result_t.link_speed`
typedef enum {
  POLLING_TEST_LINK_FULL_SPEED = 0,
  POLLING_TEST_LINK_HIGH_SPEED,
} polling_test_link_speed_t;

typedef enum {
  // Begin a measurement window
  POLLING_TEST_SUBCMD_START = 0,
  // Read the current state and, once finished, the results
  POLLING_TEST_SUBCMD_POLL,
  // Stop a window that is still in progress
  POLLING_TEST_SUBCMD_ABORT,

  POLLING_TEST_SUBCMD_COUNT,
} polling_test_subcmd_t;

typedef enum {
  // No window has been run since power-on
  POLLING_TEST_STATUS_IDLE = 0,
  // A window is in progress; the results are not valid yet
  POLLING_TEST_STATUS_RUNNING,
  // The window finished normally and the results are valid
  POLLING_TEST_STATUS_DONE,
  // Refused because a window is already in progress
  POLLING_TEST_STATUS_ERR_BUSY,
  // Refused because keys are held. Sending empty reports would release them on
  // the host without the firmware noticing.
  POLLING_TEST_STATUS_ERR_KEYS_HELD,
  // Refused, or given up, because the endpoint never became ready
  POLLING_TEST_STATUS_ERR_NOT_READY,
  // The bus went away in the middle of the window
  POLLING_TEST_STATUS_ABORTED_BUS,
  // The bus was suspended in the middle of the window
  POLLING_TEST_STATUS_ABORTED_SUSPEND,
  // The host asked to stop
  POLLING_TEST_STATUS_ABORTED_HOST,
} polling_test_status_t;

// Result of a measurement window. Sent verbatim to the configurator as the
// payload of `COMMAND_POLLING_TEST`, so the layout is part of the protocol.
typedef struct __attribute__((packed)) {
  // `polling_test_status_t`
  uint8_t status;
  // `polling_test_link_speed_t`, as negotiated with the host
  uint8_t link_speed;
  // `bInterval` currently encoded in the configuration descriptor
  uint8_t b_interval;
  // `POLLING_TEST_FLAG_*`
  uint8_t flags;
  // Polling rate implied by `link_speed` and `b_interval`
  uint16_t target_rate_hz;
  // Requested window length after clamping
  uint16_t window_ms;
  // Time between the first and the last acknowledged report
  uint32_t elapsed_us;
  // Number of measured intervals, that is, acknowledged reports after the
  // first. The measured rate is `report_count / elapsed_us`.
  uint32_t report_count;
  // Frame counter ticks spanned by those intervals. Comparing this against the
  // expected tick count separates a host that is not polling from a host that
  // is polling but not completing the transfers.
  uint32_t sof_count;
  // Main loop iterations during the window
  uint32_t loop_count;
  // Shortest and longest interval, in frame counter ticks
  uint16_t min_gap;
  uint16_t max_gap;
  // Interval histogram, bucketed by how many polling opportunities were missed:
  // 0 | 1 | 2-3 | 4-7 | 8-15 | 16 or more. Sums to `report_count`.
  uint16_t buckets[POLLING_TEST_NUM_BUCKETS];
  // Reports the USB stack failed to deliver
  uint16_t failed;
  // Worst main loop iteration and worst acknowledgement-to-rearm turnaround
  uint32_t loop_max_cycles;
  uint32_t send_max_cycles;
  // CPU clock in MHz, to convert the cycle counts above
  uint16_t cpu_mhz;
} polling_test_result_t;

//--------------------------------------------------------------------+
// USB Polling Self-Test API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the polling self-test module
 *
 * @return None
 */
void polling_test_init(void);

/**
 * @brief Advance the polling self-test
 *
 * @return true if a measurement window is in progress, in which case the caller
 * must skip the rest of the main loop so that the measurement is not perturbed
 */
bool polling_test_task(void);

/**
 * @brief Handle a `COMMAND_POLLING_TEST` request
 *
 * @param subcommand `polling_test_subcmd_t`
 * @param window_ms Requested window length, only used by
 * `POLLING_TEST_SUBCMD_START`
 * @param out Buffer to write the current state and results to
 *
 * @return None
 */
void polling_test_command(uint8_t subcommand, uint16_t window_ms,
                          polling_test_result_t *out);

/**
 * @brief Record a delivered keyboard report and re-arm the endpoint
 *
 * Called from `tud_hid_report_complete_cb()`. Does nothing unless a measurement
 * window is in progress.
 *
 * @return None
 */
void polling_test_on_complete(void);

/**
 * @brief Record a keyboard report the USB stack failed to deliver
 *
 * Called from `tud_hid_report_failed_cb()`. Does nothing unless a measurement
 * window is in progress.
 *
 * @return None
 */
void polling_test_on_failed(void);
