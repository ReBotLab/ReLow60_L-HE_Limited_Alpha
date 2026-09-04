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

#ifdef OLED_ENABLED

#include "oled_display.h"

#include "hardware/oled_api.h"
#include "hardware/timer_api.h"
#include "matrix.h"

// Update interval in milliseconds (~30 FPS)
#define OLED_UPDATE_INTERVAL_MS 33

// Character cell of the 5x7 font: 5px glyph + 1px gap
#define CHAR_W 6
#define LINE_H 8

// State machine for non-blocking OLED update
enum { OLED_STATE_IDLE, OLED_STATE_SENDING };

static uint32_t last_update;
static uint8_t oled_state = OLED_STATE_IDLE;

#if defined(OLED_LAYOUT_TESTER)

//--------------------------------------------------------------------+
// Tester layout (32x128 portrait)
//
// Detail view: the key grid from `layout.keymap` on top, then the last
// pressed key's number, label, ADC, rest, bottom-out and travel, then the
// temperature, then a travel bar. After OLED_IDLE_TIMEOUT_MS without a key
// press the display cycles through overview pages of six keys each (label,
// key number, ADC) and a temperature page, OLED_PAGE_INTERVAL_MS per page.
// Any key press returns to the detail view showing that key.
//--------------------------------------------------------------------+

#define GRID_CELL 8 // cell pitch in px
#define GRID_SQ 7   // square size, leaving a 1px gap
#define GRID_X0 ((uint8_t)((OLED_WIDTH - OLED_GRID_COLS * GRID_CELL + 1) / 2))
#define DETAIL_TOP ((uint8_t)(OLED_GRID_ROWS * GRID_CELL + 2))
#define KEYS_PER_PAGE 6

static const int8_t key_grid[OLED_GRID_ROWS][OLED_GRID_COLS] = OLED_GRID;

#ifdef OLED_KEY_LABELS
static const char *const key_labels[OLED_NUM_KEY_LABELS] = OLED_KEY_LABELS;
#endif

#ifdef OLED_NTC_KEY
static const int16_t ntc_table[OLED_NTC_TABLE_LEN][2] = OLED_NTC_TABLE;
#define NUM_PHYSICAL_KEYS (NUM_KEYS - 1)
#define NUM_PAGES ((NUM_PHYSICAL_KEYS + KEYS_PER_PAGE - 1) / KEYS_PER_PAGE + 1)
#else
#define NUM_PHYSICAL_KEYS NUM_KEYS
#define NUM_PAGES ((NUM_PHYSICAL_KEYS + KEYS_PER_PAGE - 1) / KEYS_PER_PAGE)
#endif

enum { MODE_DETAIL, MODE_OVERVIEW };

static uint8_t mode = MODE_DETAIL;
static uint8_t selected_key;
static uint8_t page;
static uint32_t last_activity;
static uint32_t page_started;
static bool prev_pressed[NUM_KEYS];

// n-th physical key, skipping the NTC pseudo-key
static uint8_t physical_key(uint8_t n) {
#ifdef OLED_NTC_KEY
  if (n >= OLED_NTC_KEY)
    return (uint8_t)(n + 1);
#endif
  return n;
}

static bool is_physical_key(uint8_t key) {
#ifdef OLED_NTC_KEY
  if (key == OLED_NTC_KEY)
    return false;
#endif
  return true;
}

static const char *key_label(uint8_t key) {
#ifdef OLED_KEY_LABELS
  if (key < OLED_NUM_KEY_LABELS)
    return key_labels[key];
#endif
  (void)key;
  return "";
}

static uint8_t number_width(uint16_t num) {
  uint8_t digits = 1;
  for (uint16_t n = num; n >= 10; n /= 10)
    digits++;
  return (uint8_t)(digits * CHAR_W);
}

// Right-align a number so its last glyph ends at the display edge
static void draw_number_right(uint8_t y, uint16_t num, bool on) {
  oled_draw_number_color((uint8_t)(OLED_WIDTH + 1 - number_width(num)), y,
                         num, on);
}

#ifdef OLED_NTC_KEY
// Temperature in 0.1 C from the divider midpoint ADC code, by linear
// interpolation of the table built at compile time from the thermistor
// constants in keyboard.json
static int16_t ntc_temp_x10(uint16_t adc) {
  if (adc <= (uint16_t)ntc_table[0][0])
    return ntc_table[0][1];
  for (uint8_t i = 1; i < OLED_NTC_TABLE_LEN; i++) {
    if (adc <= (uint16_t)ntc_table[i][0]) {
      int32_t a0 = ntc_table[i - 1][0];
      int32_t a1 = ntc_table[i][0];
      int32_t t0 = ntc_table[i - 1][1];
      int32_t t1 = ntc_table[i][1];
      return (int16_t)(t0 + (t1 - t0) * ((int32_t)adc - a0) / (a1 - a0));
    }
  }
  return ntc_table[OLED_NTC_TABLE_LEN - 1][1];
}

// Format tenths of a degree as "23.4C" or "-5.2C"
static void format_temp(char *buf, int16_t t10) {
  uint8_t pos = 0;
  if (t10 < 0) {
    buf[pos++] = '-';
    t10 = (int16_t)-t10;
  }
  if (t10 > 999)
    t10 = 999;
  uint8_t whole = (uint8_t)(t10 / 10);
  if (whole >= 10)
    buf[pos++] = (char)('0' + whole / 10);
  buf[pos++] = (char)('0' + whole % 10);
  buf[pos++] = '.';
  buf[pos++] = (char)('0' + t10 % 10);
  buf[pos++] = 'C';
  buf[pos] = '\0';
}
#endif

static void draw_key_grid(void) {
  for (uint8_t r = 0; r < OLED_GRID_ROWS; r++) {
    for (uint8_t c = 0; c < OLED_GRID_COLS; c++) {
      int8_t key = key_grid[r][c];
      if (key < 0 || key >= NUM_KEYS)
        continue;
      uint8_t x = (uint8_t)(GRID_X0 + c * GRID_CELL);
      uint8_t y = (uint8_t)(r * GRID_CELL);
      oled_draw_rect(x, y, GRID_SQ, GRID_SQ);
      // Selected key: doubled border
      if ((uint8_t)key == selected_key)
        oled_draw_rect((uint8_t)(x + 1), (uint8_t)(y + 1), GRID_SQ - 2,
                       GRID_SQ - 2);
      // Fill from the top in proportion to travel
      uint8_t distance = key_matrix[key].distance;
      if (distance > 0) {
        uint8_t fill_h =
            (uint8_t)(1 + (uint16_t)distance * (GRID_SQ - 1) / 255);
        oled_fill_rect(x, y, GRID_SQ, fill_h, true);
      }
    }
  }
}

static void draw_detail(void) {
  draw_key_grid();

  const key_state_t *k = &key_matrix[selected_key];
  uint8_t y = DETAIL_TOP;

  // 1-based key number
  oled_draw_char(0, y, 'K');
  oled_draw_number(CHAR_W, y, (uint16_t)(selected_key + 1));
  y = (uint8_t)(y + LINE_H);

  oled_draw_string(0, y, key_label(selected_key));
  y = (uint8_t)(y + LINE_H);

  // Prefixed, right-aligned: A = ADC, R = rest, B = bottom-out, D = travel
  oled_draw_char(0, y, 'A');
  draw_number_right(y, k->adc_filtered, true);
  y = (uint8_t)(y + LINE_H);
  oled_draw_char(0, y, 'R');
  draw_number_right(y, k->adc_rest_value, true);
  y = (uint8_t)(y + LINE_H);
  oled_draw_char(0, y, 'B');
  draw_number_right(y, k->adc_bottom_out_value, true);
  y = (uint8_t)(y + LINE_H);
  oled_draw_char(0, y, 'D');
  draw_number_right(y, k->distance, true);
  y = (uint8_t)(y + LINE_H + 2);

#ifdef OLED_NTC_KEY
  char buf[8];
  format_temp(buf, ntc_temp_x10(key_matrix[OLED_NTC_KEY].adc_filtered));
  oled_draw_string(0, y, "TEMP");
  y = (uint8_t)(y + LINE_H);
  oled_draw_string(0, y, buf);
  y = (uint8_t)(y + LINE_H + 2);
#endif

  // Travel bar for the selected key in whatever height is left
  if (y + 8 < OLED_HEIGHT) {
    uint8_t bar_h = (uint8_t)(OLED_HEIGHT - 1 - y);
    uint8_t fill_h = (uint8_t)((uint16_t)k->distance * (bar_h - 2) / 255);
    oled_draw_rect(12, y, 8, bar_h);
    if (fill_h > 0)
      oled_fill_rect(13, (uint8_t)(y + bar_h - 1 - fill_h), 6, fill_h, true);
  }
}

static void draw_overview(void) {
#ifdef OLED_NTC_KEY
  if (page == NUM_PAGES - 1) {
    char buf[8];
    uint16_t adc = key_matrix[OLED_NTC_KEY].adc_filtered;
    oled_draw_string(0, 0, "TEMP");
    format_temp(buf, ntc_temp_x10(adc));
    oled_draw_string(0, 2 * LINE_H, buf);
    oled_draw_char(0, 4 * LINE_H, 'A');
    draw_number_right(4 * LINE_H, adc, true);
    return;
  }
#endif

  uint8_t first = (uint8_t)(page * KEYS_PER_PAGE);
  uint8_t last = (uint8_t)(first + KEYS_PER_PAGE);
  if (last > NUM_PHYSICAL_KEYS)
    last = NUM_PHYSICAL_KEYS;

  // Header "K1-6" in 1-based key numbers
  uint8_t x = 0;
  oled_draw_char(x, 0, 'K');
  x = (uint8_t)(x + CHAR_W);
  oled_draw_number(x, 0, (uint16_t)(first + 1));
  x = (uint8_t)(x + number_width((uint16_t)(first + 1)));
  oled_draw_char(x, 0, '-');
  x = (uint8_t)(x + CHAR_W);
  oled_draw_number(x, 0, last);

  // Two lines per key: label + key number, then ADC. Inverted while pressed.
  for (uint8_t n = first; n < last; n++) {
    uint8_t key = physical_key(n);
    uint8_t y = (uint8_t)(LINE_H + (n - first) * 2 * LINE_H);
    bool pressed = key_matrix[key].is_pressed;
    if (pressed)
      oled_fill_rect(0, y, OLED_WIDTH, 2 * LINE_H, true);
    oled_draw_string_color(0, y, key_label(key), !pressed);
    draw_number_right(y, (uint16_t)(key + 1), !pressed);
    draw_number_right((uint8_t)(y + LINE_H), key_matrix[key].adc_filtered,
                      !pressed);
  }
}

static void update_mode(void) {
  uint32_t now = timer_read();
  bool any_pressed = false;

  for (uint8_t i = 0; i < NUM_KEYS; i++) {
    if (!is_physical_key(i))
      continue;
    bool pressed = key_matrix[i].is_pressed;
    if (pressed && !prev_pressed[i]) {
      selected_key = i;
      mode = MODE_DETAIL;
    }
    if (pressed)
      any_pressed = true;
    prev_pressed[i] = pressed;
  }

  if (any_pressed)
    last_activity = now;

  if (mode == MODE_DETAIL) {
    if (timer_elapsed(last_activity) >= OLED_IDLE_TIMEOUT_MS) {
      mode = MODE_OVERVIEW;
      page = 0;
      page_started = now;
    }
  } else if (timer_elapsed(page_started) >= OLED_PAGE_INTERVAL_MS) {
    page = (uint8_t)((page + 1) % NUM_PAGES);
    page_started = now;
  }
}

static void layout_init(void) {
  selected_key = physical_key(0);
  last_activity = timer_read();
  page_started = last_activity;
}

static void render_frame(void) {
  update_mode();
  oled_clear();
  if (mode == MODE_DETAIL)
    draw_detail();
  else
    draw_overview();
}

#else // OLED_LAYOUT_GRID3X3

//--------------------------------------------------------------------+
// 3x3 grid layout (32x128 portrait)
//
// Top: 3x3 grid of key shapes with fill animation
// Bottom: ADC values in a single column (inverted when pressed)
//
// Key rectangle size: 8x8 pixels
// Grid spacing: 10px horizontal, 10px vertical
//--------------------------------------------------------------------+

#define KEY_W 8
#define KEY_H 8
#define GRID_X 1
#define GRID_Y 2
#define GRID_STEP_X 10
#define GRID_STEP_Y 10

// ADC number display area (single column below key grid)
#define NUM_Y 38
#define NUM_STEP_Y 10
#define NUM_LINE_H 8

static void layout_init(void) {}

static void render_frame(void) {
  oled_clear();

  uint8_t num_keys = NUM_KEYS;
  if (num_keys > 9)
    num_keys = 9;

  for (uint8_t i = 0; i < num_keys; i++) {
    uint8_t col = i / 3;
    uint8_t row = i % 3;

    uint8_t kx = GRID_X + col * GRID_STEP_X;
    uint8_t ky = GRID_Y + row * GRID_STEP_Y;

    // Key fill animation: fill from top proportional to distance
    oled_draw_rect(kx, ky, KEY_W, KEY_H);
    uint8_t distance = key_matrix[i].distance;
    if (distance > 0) {
      uint8_t fill_h = (uint8_t)(1 + (uint16_t)distance * (KEY_H - 1) / 255);
      oled_fill_rect(kx, ky, KEY_W, fill_h, true);
    }

    // ADC value display with key index label
    uint8_t ny = NUM_Y + i * NUM_STEP_Y;
    if (key_matrix[i].is_pressed) {
      // Inverted display when pressed
      oled_fill_rect(0, ny, OLED_WIDTH, NUM_LINE_H, true);
      oled_draw_char_color(0, ny, '1' + i, false);
      oled_draw_number_color(8, ny, key_matrix[i].adc_filtered, false);
    } else {
      oled_draw_char(0, ny, '1' + i);
      oled_draw_number(8, ny, key_matrix[i].adc_filtered);
    }
  }
}

#endif

void oled_display_init(void) {
  oled_init();
  last_update = timer_read();
  layout_init();
}

void oled_display_task(void) {
  switch (oled_state) {
  case OLED_STATE_IDLE:
    if (timer_elapsed(last_update) < OLED_UPDATE_INTERVAL_MS)
      return;
    last_update = timer_read();

    render_frame();

    // Start DMA page transfers (sends commands, then kicks off page 0)
    oled_update();
    oled_state = OLED_STATE_SENDING;
    // Fall through to immediately try sending first page
    __attribute__((fallthrough));

  case OLED_STATE_SENDING:
    if (oled_update_poll())
      oled_state = OLED_STATE_IDLE;
    return;

  default:
    break;
  }
}

#endif
