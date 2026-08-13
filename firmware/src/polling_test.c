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

#include "polling_test.h"

#include "eeconfig.h"
#include "hardware/hardware.h"
#include "hid.h"
#include "tusb.h"
#include "usb_descriptors.h"

#define CYCLES_PER_MS (F_CPU / 1000u)

typedef enum {
  // No window in progress
  PT_STATE_IDLE = 0,
  // Waiting for the keyboard endpoint to accept the first report
  PT_STATE_ARMED,
  // Measuring
  PT_STATE_RUNNING,
} pt_state_t;

static pt_state_t state;
static polling_test_result_t result;

// An empty report releases nothing and presses nothing, so the host sees no key
// event no matter how many of these are sent.
static const hid_nkro_kb_report_t empty_report;

// Frame counter ticks between polls at the configured rate
static uint16_t expected_gap;
// Frame counter value at the previous acknowledged report
static uint16_t last_sof;
// Frame counter ticks accumulated over all measured intervals
static uint32_t sof_accumulator;
// Whether the first report has been acknowledged, which anchors the window
static bool anchored;

static uint32_t window_cycles;
static uint32_t stall_cycles;
static uint32_t arm_cycles;
static uint32_t anchor_cycles;
static uint32_t last_complete_cycles;
static uint32_t last_loop_cycles;

/**
 * @brief Finish the current window
 *
 * @param status Terminal status to report to the host
 *
 * @return None
 */
static void polling_test_finish(uint8_t status) {
  state = PT_STATE_IDLE;
  result.status = status;

  if (result.report_count == 0)
    // No interval was measured, so leave the sentinel out of the result
    result.min_gap = 0;

  // The host's view of the keyboard has been overwritten with empty reports, so
  // make sure the next real report is not suppressed as unchanged.
  hid_keyboard_invalidate_cache();
}

/**
 * @brief Begin a measurement window
 *
 * @param window_ms Requested window length in milliseconds
 *
 * @return None
 */
static void polling_test_start(uint16_t window_ms) {
  if (window_ms < POLLING_TEST_MIN_WINDOW_MS)
    window_ms = POLLING_TEST_MIN_WINDOW_MS;
  else if (window_ms > POLLING_TEST_MAX_WINDOW_MS)
    window_ms = POLLING_TEST_MAX_WINDOW_MS;

  const uint8_t b_interval = usb_descriptors_polling_interval();
  const bool high_speed = tud_speed_get() == TUSB_SPEED_HIGH;

  memset(&result, 0, sizeof(result));
  result.status = POLLING_TEST_STATUS_RUNNING;
  result.link_speed = high_speed ? POLLING_TEST_LINK_HIGH_SPEED
                                 : POLLING_TEST_LINK_FULL_SPEED;
  result.b_interval = b_interval;
  result.flags = eeconfig->options.xinput_enabled
                     ? POLLING_TEST_FLAG_XINPUT_ENABLED
                     : 0;
  result.window_ms = window_ms;
  result.min_gap = UINT16_MAX;
  result.cpu_mhz = (uint16_t)(F_CPU / 1000000u);

  if (high_speed) {
    // `bInterval` is an exponent and one tick is one 125us microframe
    expected_gap = (uint16_t)(1u << (b_interval - 1));
    result.target_rate_hz = (uint16_t)(8000u >> (b_interval - 1));
  } else {
    // `bInterval` is a frame count and one tick is one 1ms frame
    expected_gap = b_interval;
    result.target_rate_hz = (uint16_t)(1000u / b_interval);
  }

  sof_accumulator = 0;
  anchored = false;
  window_cycles = (uint32_t)window_ms * CYCLES_PER_MS;
  stall_cycles = POLLING_TEST_STALL_MS * CYCLES_PER_MS;
  arm_cycles = board_cycle_count();
  last_complete_cycles = arm_cycles;
  last_loop_cycles = arm_cycles;
  state = PT_STATE_ARMED;
}

void polling_test_init(void) {
  state = PT_STATE_IDLE;
  memset(&result, 0, sizeof(result));
  result.status = POLLING_TEST_STATUS_IDLE;
}

bool polling_test_task(void) {
  if (state == PT_STATE_IDLE)
    return false;

  if (!tud_mounted()) {
    polling_test_finish(POLLING_TEST_STATUS_ABORTED_BUS);
    return false;
  }

  if (tud_suspended()) {
    // Deliberately not waking the host up: a suspended bus cannot be measured,
    // and resuming it would hide the very problem we are looking for.
    polling_test_finish(POLLING_TEST_STATUS_ABORTED_SUSPEND);
    return false;
  }

  const uint32_t now = board_cycle_count();

  if (state == PT_STATE_ARMED) {
    if (tud_hid_n_ready(USB_ITF_KEYBOARD)) {
      state = PT_STATE_RUNNING;
      last_complete_cycles = now;
      last_loop_cycles = now;
      (void)tud_hid_n_report(USB_ITF_KEYBOARD, 0, &empty_report,
                             sizeof(empty_report));
    } else if (now - arm_cycles > stall_cycles) {
      polling_test_finish(POLLING_TEST_STATUS_ERR_NOT_READY);
      return false;
    }

    return true;
  }

  result.loop_count++;
  const uint32_t loop_cycles = now - last_loop_cycles;
  last_loop_cycles = now;
  if (loop_cycles > result.loop_max_cycles)
    result.loop_max_cycles = loop_cycles;

  if (now - last_complete_cycles > stall_cycles) {
    // The host stopped servicing the endpoint entirely
    polling_test_finish(POLLING_TEST_STATUS_ABORTED_BUS);
    return false;
  }

  if (anchored && now - anchor_cycles >= window_cycles) {
    result.elapsed_us = (last_complete_cycles - anchor_cycles) / result.cpu_mhz;
    result.sof_count = sof_accumulator;
    polling_test_finish(POLLING_TEST_STATUS_DONE);
    return false;
  }

  return true;
}

void polling_test_command(uint8_t subcommand, uint16_t window_ms,
                          polling_test_result_t *out) {
  switch (subcommand) {
  case POLLING_TEST_SUBCMD_START:
    if (state != PT_STATE_IDLE) {
      // Report the refusal without disturbing the window in progress
      *out = result;
      out->status = POLLING_TEST_STATUS_ERR_BUSY;
      return;
    }

    if (!tud_mounted() || tud_suspended()) {
      *out = result;
      out->status = POLLING_TEST_STATUS_ERR_NOT_READY;
      return;
    }

    if (!hid_keyboard_is_idle()) {
      // Empty reports would release the held keys on the host
      *out = result;
      out->status = POLLING_TEST_STATUS_ERR_KEYS_HELD;
      return;
    }

    polling_test_start(window_ms);
    break;

  case POLLING_TEST_SUBCMD_ABORT:
    if (state != PT_STATE_IDLE)
      polling_test_finish(POLLING_TEST_STATUS_ABORTED_HOST);
    break;

  case POLLING_TEST_SUBCMD_POLL:
  default:
    break;
  }

  *out = result;
}

void polling_test_on_complete(void) {
  if (state != PT_STATE_RUNNING)
    return;

  const uint32_t now = board_cycle_count();
  const uint16_t sof = (uint16_t)board_usb_frame_number();

  if (!anchored) {
    // Anchor on the first acknowledgement so that the time spent arming the
    // endpoint is not counted, and discard the meaningless first interval.
    anchored = true;
    anchor_cycles = now;
    last_sof = sof;
  } else {
    const uint16_t gap =
        (uint16_t)((sof - last_sof) & USB_FRAME_NUMBER_MASK);
    last_sof = sof;
    sof_accumulator += gap;

    if (gap < result.min_gap)
      result.min_gap = gap;
    if (gap > result.max_gap)
      result.max_gap = gap;

    // How many polling opportunities went by without a report
    const uint32_t periods = gap / expected_gap;
    const uint32_t missed = periods > 0 ? periods - 1 : 0;
    uint32_t bucket;
    if (missed == 0)
      bucket = 0;
    else if (missed == 1)
      bucket = 1;
    else if (missed <= 3)
      bucket = 2;
    else if (missed <= 7)
      bucket = 3;
    else if (missed <= 15)
      bucket = 4;
    else
      bucket = 5;

    if (result.buckets[bucket] < UINT16_MAX)
      result.buckets[bucket]++;
    result.report_count++;
  }

  last_complete_cycles = now;

  // Re-arm right away. TinyUSB clears the endpoint's busy flag before invoking
  // this callback, so the claim always succeeds.
  (void)tud_hid_n_report(USB_ITF_KEYBOARD, 0, &empty_report,
                         sizeof(empty_report));

  const uint32_t send_cycles = board_cycle_count() - now;
  if (send_cycles > result.send_max_cycles)
    result.send_max_cycles = send_cycles;
}

void polling_test_on_failed(void) {
  if (state != PT_STATE_RUNNING)
    return;

  if (result.failed < UINT16_MAX)
    result.failed++;

  // The endpoint is idle again after a failed transfer, so keep the window
  // going rather than stalling until the watchdog fires.
  (void)tud_hid_n_report(USB_ITF_KEYBOARD, 0, &empty_report,
                         sizeof(empty_report));
}
