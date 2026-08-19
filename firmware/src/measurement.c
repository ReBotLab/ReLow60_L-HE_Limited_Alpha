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

#include "measurement.h"

#include "hardware/hardware.h"
#include "matrix.h"
#include "polling_test.h"
#include "tusb.h"

#include "at32f402_405.h"

#include <stdio.h>
#include <string.h>

// CDC command buffer
#define CMD_BUF_SIZE 32
static char cmd_buf[CMD_BUF_SIZE];
static uint8_t cmd_len = 0;

// ADC sample buffer (shared between noise and settling measurement)
static uint16_t sample_buf[MEASUREMENT_NUM_SAMPLES];

// State machine
enum {
  MEAS_IDLE,
  MEAS_CAPTURE,
  MEAS_SEND,
  MEAS_SETTLING_CAPTURE,
  MEAS_SETTLING_SEND,
  MEAS_AVG_CAPTURE,
  MEAS_LOOPTIME,
  MEAS_STREAM,
  MEAS_POLLING,
};

// Window used by the `polling` command
#define MEASUREMENT_POLLING_WINDOW_MS 500

static uint8_t meas_state = MEAS_IDLE;
static uint8_t current_key;
static uint16_t sample_idx;
static uint16_t send_idx;
static uint16_t current_num_samples;

// Looptime measurement state
static uint32_t looptime_last_cyc;
static uint32_t looptime_min;
static uint32_t looptime_max;
static uint32_t looptime_total;

static uint32_t isqrt32(uint32_t n) {
  if (n == 0)
    return 0;
  uint32_t x = n, y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + n / x) / 2;
  }
  return x;
}

// Print a string to CDC (blocking: waits for TX FIFO space)
static void cdc_print(const char *str) {
  uint32_t len = (uint32_t)strlen(str);
  uint32_t sent = 0;
  while (sent < len) {
    if (!tud_cdc_connected())
      return;
    uint32_t written = tud_cdc_write(str + sent, len - sent);
    sent += written;
    if (written == 0) {
      tud_cdc_write_flush();
      tud_task();
    }
  }
}

static void cdc_flush(void) {
  if (tud_cdc_connected())
    tud_cdc_write_flush();
}

// Process a received command line
static void process_command(const char *cmd) {
  if (strcmp(cmd, "noise") == 0) {
    // Start noise measurement for all keys
    cdc_print("# Noise measurement: ");
    char num[8];
    snprintf(num, sizeof(num), "%d", NUM_KEYS);
    cdc_print(num);
    cdc_print(" keys x ");
    snprintf(num, sizeof(num), "%d", MEASUREMENT_NUM_SAMPLES);
    cdc_print(num);
    cdc_print(" samples\r\n");
    cdc_flush();
    current_key = 0;
    sample_idx = 0;
    meas_state = MEAS_CAPTURE;
  } else if (strcmp(cmd, "status") == 0) {
    // Print current ADC values for all keys
    cdc_print("# key,adc_raw,adc_filtered,distance,pressed\r\n");
    for (uint8_t i = 0; i < NUM_KEYS; i++) {
      char line[64];
      snprintf(line, sizeof(line), "%d,%d,%d,%d,%d\r\n", i + 1,
               analog_read(i), key_matrix[i].adc_filtered,
               key_matrix[i].distance, key_matrix[i].is_pressed);
      cdc_print(line);
    }
    cdc_flush();
  } else if (strcmp(cmd, "settling") == 0) {
#if ADC_NUM_MUX_INPUTS > 0
    cdc_print("# Settling measurement: ");
    char num[8];
    snprintf(num, sizeof(num), "%d", NUM_KEYS);
    cdc_print(num);
    cdc_print(" keys x ");
    snprintf(num, sizeof(num), "%d", MEASUREMENT_SETTLING_SAMPLES);
    cdc_print(num);
    cdc_print(" samples\r\n");
    cdc_flush();
    current_key = 0;
    current_num_samples = MEASUREMENT_SETTLING_SAMPLES;
    meas_state = MEAS_SETTLING_CAPTURE;
#else
    cdc_print("# No MUX configured\r\n");
    cdc_flush();
#endif
  } else if (strcmp(cmd, "avg") == 0) {
    cdc_print("# key,mean,min,max,stdev\r\n");
    cdc_flush();
    current_key = 0;
    sample_idx = 0;
    meas_state = MEAS_AVG_CAPTURE;
  } else if (strncmp(cmd, "stream ", 7) == 0) {
    int key_num = 0;
    const char *p = cmd + 7;
    while (*p >= '0' && *p <= '9') {
      key_num = key_num * 10 + (*p - '0');
      p++;
    }
    if (key_num < 1 || key_num > NUM_KEYS) {
      char usage[48];
      snprintf(usage, sizeof(usage), "# Usage: stream <1-%d>\r\n", NUM_KEYS);
      cdc_print(usage);
      cdc_flush();
      return;
    }
    uint8_t key = (uint8_t)(key_num - 1);
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "# Streaming key %d (Enter to stop)\r\n",
             key + 1);
    cdc_print(hdr);
    cdc_flush();
    current_key = key;
    meas_state = MEAS_STREAM;
  } else if (strcmp(cmd, "looptime") == 0) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    looptime_min = 0xFFFFFFFF;
    looptime_max = 0;
    looptime_total = 0;
    sample_idx = 0;
    looptime_last_cyc = DWT->CYCCNT;
    meas_state = MEAS_LOOPTIME;
  } else if (strcmp(cmd, "polling") == 0) {
    polling_test_result_t r;
    polling_test_command(POLLING_TEST_SUBCMD_START,
                         MEASUREMENT_POLLING_WINDOW_MS, &r);
    if (r.status != POLLING_TEST_STATUS_RUNNING) {
      char line[64];
      snprintf(line, sizeof(line), "# Could not start (status %d)\r\n",
               r.status);
      cdc_print(line);
      cdc_flush();
      return;
    }
    cdc_print("# USB polling measurement\r\n");
    cdc_flush();
    meas_state = MEAS_POLLING;
  } else if (strcmp(cmd, "help") == 0) {
    cdc_print("# Commands:\r\n");
    cdc_print("#   noise    - capture 1000 ADC samples per key (CSV)\r\n");
    cdc_print("#   settling - capture 200 rapid samples after MUX switch\r\n");
    cdc_print("#   avg      - 100-sample average per key (mean/min/max/stdev)\r\n");
    cdc_print("#   stream N - continuous ADC output for key N (Enter to stop)\r\n");
    cdc_print("#   looptime - measure main loop timing (1000 iterations)\r\n");
    cdc_print("#   polling  - measure USB polling rate (500 ms window)\r\n");
    cdc_print("#   status   - show current ADC values\r\n");
    cdc_print("#   help     - show this message\r\n");
    cdc_flush();
  } else {
    cdc_print("# Unknown command. Type 'help' for usage.\r\n");
    cdc_flush();
  }
}

void measurement_task(void) {
  // Read CDC input
  if (tud_cdc_available()) {
    while (tud_cdc_available()) {
      char c = (char)tud_cdc_read_char();
      if (c == '\r' || c == '\n') {
        if (cmd_len > 0) {
          cmd_buf[cmd_len] = '\0';
          process_command(cmd_buf);
          cmd_len = 0;
        }
      } else if (cmd_len < CMD_BUF_SIZE - 1) {
        cmd_buf[cmd_len++] = c;
      }
    }
  }

  switch (meas_state) {
  case MEAS_CAPTURE:
    // Capture raw ADC samples as fast as possible
    sample_buf[sample_idx] = analog_read(current_key);
    sample_idx++;
    if (sample_idx >= MEASUREMENT_NUM_SAMPLES) {
      // Done capturing this key, start sending
      meas_state = MEAS_SEND;
      send_idx = 0;
      // Print CSV header for this key
      char hdr[32];
      snprintf(hdr, sizeof(hdr), "# key %d\r\n", current_key + 1);
      cdc_print(hdr);
      cdc_flush();
    }
    break;

  case MEAS_SEND:
    // Send buffered samples via CDC (batch to avoid blocking too long)
    if (!tud_cdc_connected()) {
      meas_state = MEAS_IDLE;
      break;
    }
    for (uint8_t batch = 0; batch < 50 && send_idx < MEASUREMENT_NUM_SAMPLES;
         batch++, send_idx++) {
      char line[16];
      snprintf(line, sizeof(line), "%d\r\n", sample_buf[send_idx]);
      cdc_print(line);
    }
    cdc_flush();

    if (send_idx >= MEASUREMENT_NUM_SAMPLES) {
      // Move to next key
      current_key++;
      if (current_key >= NUM_KEYS) {
        cdc_print("# Done\r\n");
        cdc_flush();
        meas_state = MEAS_IDLE;
      } else {
        // Capture next key
        sample_idx = 0;
        meas_state = MEAS_CAPTURE;
      }
    }
    break;

  case MEAS_SETTLING_CAPTURE:
#if ADC_NUM_MUX_INPUTS > 0
    analog_settling_capture(current_key, sample_buf, current_num_samples);
#endif
    meas_state = MEAS_SETTLING_SEND;
    send_idx = 0;
    {
      char hdr[32];
      snprintf(hdr, sizeof(hdr), "# key %d\r\n", current_key + 1);
      cdc_print(hdr);
      cdc_flush();
    }
    break;

  case MEAS_SETTLING_SEND:
    if (!tud_cdc_connected()) {
      meas_state = MEAS_IDLE;
      break;
    }
    for (uint8_t batch = 0; batch < 50 && send_idx < current_num_samples;
         batch++, send_idx++) {
      char line[16];
      snprintf(line, sizeof(line), "%d\r\n", sample_buf[send_idx]);
      cdc_print(line);
    }
    cdc_flush();

    if (send_idx >= current_num_samples) {
      current_key++;
      if (current_key >= NUM_KEYS) {
        cdc_print("# Done\r\n");
        cdc_flush();
        meas_state = MEAS_IDLE;
      } else {
        meas_state = MEAS_SETTLING_CAPTURE;
      }
    }
    break;

  case MEAS_AVG_CAPTURE:
    sample_buf[sample_idx] = analog_read(current_key);
    sample_idx++;
    if (sample_idx >= MEASUREMENT_AVG_SAMPLES) {
      uint32_t sum = 0;
      uint16_t vmin = 4095, vmax = 0;
      for (uint16_t i = 0; i < MEASUREMENT_AVG_SAMPLES; i++) {
        sum += sample_buf[i];
        if (sample_buf[i] < vmin)
          vmin = sample_buf[i];
        if (sample_buf[i] > vmax)
          vmax = sample_buf[i];
      }
      uint16_t mean = (uint16_t)(sum / MEASUREMENT_AVG_SAMPLES);
      uint32_t sq_sum = 0;
      for (uint16_t i = 0; i < MEASUREMENT_AVG_SAMPLES; i++) {
        int32_t d = (int32_t)sample_buf[i] - mean;
        sq_sum += (uint32_t)(d * d);
      }
      uint32_t stdev_x10 = isqrt32(sq_sum * 100 / MEASUREMENT_AVG_SAMPLES);
      char line[48];
      snprintf(line, sizeof(line), "%d,%d,%d,%d,%lu.%lu\r\n", current_key + 1,
               mean, vmin, vmax, (unsigned long)(stdev_x10 / 10),
               (unsigned long)(stdev_x10 % 10));
      cdc_print(line);
      cdc_flush();
      current_key++;
      if (current_key >= NUM_KEYS) {
        cdc_print("# Done\r\n");
        cdc_flush();
        meas_state = MEAS_IDLE;
      } else {
        sample_idx = 0;
      }
    }
    break;

  case MEAS_STREAM:
    if (tud_cdc_available()) {
      while (tud_cdc_available())
        tud_cdc_read_char();
      cdc_print("# Done\r\n");
      cdc_flush();
      meas_state = MEAS_IDLE;
      break;
    }
    {
      char line[16];
      snprintf(line, sizeof(line), "%d\r\n", analog_read(current_key));
      cdc_print(line);
    }
    break;

  case MEAS_LOOPTIME: {
    uint32_t now = DWT->CYCCNT;
    uint32_t delta = now - looptime_last_cyc;
    looptime_last_cyc = now;
    if (sample_idx > 0) {
      if (delta < looptime_min)
        looptime_min = delta;
      if (delta > looptime_max)
        looptime_max = delta;
      looptime_total += delta;
    }
    sample_idx++;
    if (sample_idx > MEASUREMENT_LOOPTIME_ITERATIONS) {
      uint32_t mhz = F_CPU / 1000000;
      uint32_t avg_cyc = looptime_total / MEASUREMENT_LOOPTIME_ITERATIONS;
      char line[80];
      snprintf(line, sizeof(line), "# loop_avg: %lu us (%lu cyc)\r\n",
               (unsigned long)(avg_cyc / mhz), (unsigned long)avg_cyc);
      cdc_print(line);
      snprintf(line, sizeof(line), "# loop_min: %lu us (%lu cyc)\r\n",
               (unsigned long)(looptime_min / mhz),
               (unsigned long)looptime_min);
      cdc_print(line);
      snprintf(line, sizeof(line), "# loop_max: %lu us (%lu cyc)\r\n",
               (unsigned long)(looptime_max / mhz),
               (unsigned long)looptime_max);
      cdc_print(line);
      snprintf(line, sizeof(line), "# scan_cycle: %d us (%d ch x %d us)\r\n",
               (1 << ADC_NUM_MUX_SELECT_PINS) * ADC_SAMPLE_DELAY,
               1 << ADC_NUM_MUX_SELECT_PINS, ADC_SAMPLE_DELAY);
      cdc_print(line);
      cdc_print("# target_8khz: 125 us\r\n");
      cdc_flush();
      meas_state = MEAS_IDLE;
    }
    break;
  }

  case MEAS_POLLING: {
    polling_test_result_t r;
    polling_test_command(POLLING_TEST_SUBCMD_POLL, 0, &r);
    if (r.status == POLLING_TEST_STATUS_RUNNING)
      // Still measuring. This state is not reached while the window is in
      // progress anyway, since the polling test owns the main loop.
      break;

    char line[80];
    if (r.status != POLLING_TEST_STATUS_DONE) {
      snprintf(line, sizeof(line), "# aborted: status %d\r\n", r.status);
      cdc_print(line);
      cdc_flush();
      meas_state = MEAS_IDLE;
      break;
    }

    const uint32_t rate =
        r.elapsed_us > 0
            ? (uint32_t)((uint64_t)r.report_count * 1000000u / r.elapsed_us)
            : 0;
    const uint32_t tick_ns =
        r.link_speed == POLLING_TEST_LINK_HIGH_SPEED ? 125000u : 1000000u;

    snprintf(line, sizeof(line), "# link: %s, bInterval: %d, target: %d Hz\r\n",
             r.link_speed == POLLING_TEST_LINK_HIGH_SPEED ? "HS" : "FS",
             r.b_interval, r.target_rate_hz);
    cdc_print(line);
    snprintf(line, sizeof(line), "# rate: %lu Hz (%lu reports / %lu us)\r\n",
             (unsigned long)rate, (unsigned long)r.report_count,
             (unsigned long)r.elapsed_us);
    cdc_print(line);
    snprintf(line, sizeof(line), "# sof: %lu ticks (expected %lu)\r\n",
             (unsigned long)r.sof_count,
             (unsigned long)((uint64_t)r.elapsed_us * 1000u / tick_ns));
    cdc_print(line);
    snprintf(line, sizeof(line), "# gap: min %d, max %d ticks\r\n", r.min_gap,
             r.max_gap);
    cdc_print(line);
    snprintf(line, sizeof(line), "# hist: %d %d %d %d %d %d\r\n", r.buckets[0],
             r.buckets[1], r.buckets[2], r.buckets[3], r.buckets[4],
             r.buckets[5]);
    cdc_print(line);
    snprintf(line, sizeof(line), "# failed: %d\r\n", r.failed);
    cdc_print(line);
    snprintf(line, sizeof(line),
             "# loop: %lu iters, max %lu cyc, rearm max %lu cyc\r\n",
             (unsigned long)r.loop_count, (unsigned long)r.loop_max_cycles,
             (unsigned long)r.send_max_cycles);
    cdc_print(line);
    snprintf(line, sizeof(line), "# budget: %lu cyc per poll\r\n",
             (unsigned long)(r.target_rate_hz > 0 ? F_CPU / r.target_rate_hz
                                                  : 0));
    cdc_print(line);
    cdc_flush();
    meas_state = MEAS_IDLE;
    break;
  }

  default:
    break;
  }
}
