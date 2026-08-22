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

#include "scan_rate.h"

#include "hardware/hardware.h"

#define CYCLES_PER_US (F_CPU / 1000000u)
#define WINDOW_CYCLES ((F_CPU / 1000u) * SCAN_RATE_WINDOW_MS)

// Full analog sweeps per second. The mux walks every channel once per sweep,
// spending `ADC_SAMPLE_DELAY` microseconds on each.
#if ADC_NUM_MUX_INPUTS > 0
#define SWEEP_PERIOD_US ((1u << ADC_NUM_MUX_SELECT_PINS) * ADC_SAMPLE_DELAY)
#else
// Without a mux every input is sampled directly, so one delay is a full sweep.
#define SWEEP_PERIOD_US (ADC_SAMPLE_DELAY)
#endif

static uint32_t window_start_cycles;
static uint32_t last_iteration_cycles;
static uint32_t iterations;
static uint32_t min_cycles;
static uint32_t max_cycles;

// Published result of the last completed window
static uint32_t eval_rate_hz;
static uint16_t eval_min_us;
static uint16_t eval_max_us;

static void scan_rate_reset_window(uint32_t now) {
  window_start_cycles = now;
  last_iteration_cycles = now;
  iterations = 0;
  min_cycles = UINT32_MAX;
  max_cycles = 0;
}

void scan_rate_init(void) {
  eval_rate_hz = 0;
  eval_min_us = 0;
  eval_max_us = 0;
  scan_rate_reset_window(board_cycle_count());
}

void scan_rate_task(void) {
  const uint32_t now = board_cycle_count();
  const uint32_t elapsed = now - last_iteration_cycles;
  last_iteration_cycles = now;

  // The first iteration after a gap (a finished polling test, say) would
  // otherwise be counted as one very long scan.
  if (iterations > 0) {
    if (elapsed < min_cycles)
      min_cycles = elapsed;
    if (elapsed > max_cycles)
      max_cycles = elapsed;
  }
  iterations++;

  const uint32_t window = now - window_start_cycles;
  if (window < WINDOW_CYCLES)
    return;

  // iterations / (window / F_CPU), kept in integer arithmetic. `window` is at
  // least SCAN_RATE_WINDOW_MS worth of cycles, so dividing it down first keeps
  // the numerator from overflowing.
  eval_rate_hz = (uint32_t)(((uint64_t)iterations * F_CPU) / window);
  eval_min_us =
      (uint16_t)(min_cycles == UINT32_MAX
                     ? 0
                     : M_MIN(min_cycles / CYCLES_PER_US, UINT16_MAX));
  eval_max_us = (uint16_t)M_MIN(max_cycles / CYCLES_PER_US, UINT16_MAX);

  scan_rate_reset_window(now);
}

void scan_rate_get(scan_rate_t *out) {
  out->eval_rate_hz = eval_rate_hz;
  out->sweep_rate_hz = 1000000u / SWEEP_PERIOD_US;
  out->sweep_period_us = SWEEP_PERIOD_US;
  out->eval_min_us = eval_min_us;
  out->eval_max_us = eval_max_us;
}
