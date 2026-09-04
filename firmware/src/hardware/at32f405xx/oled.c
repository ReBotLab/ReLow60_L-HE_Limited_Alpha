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

#include "hardware/oled_api.h"

// Framebuffer: 128x32 pixels, 1 bit per pixel, organized in pages (8 rows per
// page). Page 0 = rows 0-7, Page 1 = rows 8-15, etc.
// Each byte represents a vertical column of 8 pixels within a page.
static uint8_t framebuffer[OLED_BUFFER_SIZE];

// 5x7 bitmap font: space and digits 0-9. Each glyph is 5 columns wide; each
// column byte holds 7 rows with bit 0 at the top.
static const uint8_t font_5x7[][5] = {
    // ' ' (space)
    {0x00, 0x00, 0x00, 0x00, 0x00},
    // '0'
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    // '1'
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    // '2'
    {0x42, 0x61, 0x51, 0x49, 0x46},
    // '3'
    {0x21, 0x41, 0x45, 0x4B, 0x31},
    // '4'
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    // '5'
    {0x27, 0x45, 0x45, 0x45, 0x39},
    // '6'
    {0x3C, 0x4A, 0x49, 0x49, 0x30},
    // '7'
    {0x01, 0x71, 0x09, 0x05, 0x03},
    // '8'
    {0x36, 0x49, 0x49, 0x49, 0x36},
    // '9'
    {0x06, 0x49, 0x49, 0x29, 0x1E},
};

// Upper-case letters in the same format. Lower-case input is drawn with these.
static const uint8_t font_5x7_alpha[26][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
};

static const uint8_t font_5x7_minus[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
static const uint8_t font_5x7_period[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
static const uint8_t font_5x7_colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};

static const uint8_t *font_glyph(char c) {
  if (c >= '0' && c <= '9')
    return font_5x7[c - '0' + 1];
  if (c >= 'A' && c <= 'Z')
    return font_5x7_alpha[c - 'A'];
  if (c >= 'a' && c <= 'z')
    return font_5x7_alpha[c - 'a'];
  switch (c) {
  case '-':
    return font_5x7_minus;
  case '.':
    return font_5x7_period;
  case ':':
    return font_5x7_colon;
  default:
    return font_5x7[0]; // space for anything else
  }
}

static void oled_send_cmd(uint8_t cmd) {
  uint8_t buf[2] = {0x00, cmd}; // Co=0, D/C#=0 (command)
  oled_i2c_write(OLED_I2C_ADDR, buf, 2);
}

void oled_init(void) {
  oled_i2c_init();

  // SSD1306 initialization sequence for 128x32
  oled_send_cmd(0xAE); // Display OFF
  oled_send_cmd(0xD5); // Set display clock divide ratio
  oled_send_cmd(0x80); // Suggested ratio
  oled_send_cmd(0xA8); // Set multiplex ratio
  oled_send_cmd(0x1F); // 1/32 duty (32 rows)
  oled_send_cmd(0xD3); // Set display offset
  oled_send_cmd(0x00); // No offset
  oled_send_cmd(0x40); // Set start line to 0
  oled_send_cmd(0x8D); // Charge pump
  oled_send_cmd(0x14); // Enable charge pump
  oled_send_cmd(0x20); // Memory addressing mode
  oled_send_cmd(0x00); // Horizontal addressing mode
  oled_send_cmd(0xA1); // Segment re-map (column 127 mapped to SEG0)
  oled_send_cmd(0xC8); // COM output scan direction (remapped)
  oled_send_cmd(0xDA); // Set COM pins hardware configuration
  oled_send_cmd(0x02); // Sequential COM pin, disable remap
  oled_send_cmd(0x81); // Set contrast
  oled_send_cmd(0x8F); // Medium contrast
  oled_send_cmd(0xD9); // Set pre-charge period
  oled_send_cmd(0xF1); // Phase 1=15, Phase 2=1
  oled_send_cmd(0xDB); // Set VCOMH deselect level
  oled_send_cmd(0x40); // ~0.77 x VCC
  oled_send_cmd(0xA4); // Entire display ON (resume from GDDRAM)
  oled_send_cmd(0xA6); // Normal display (not inverted)
  oled_send_cmd(0xAF); // Display ON

  oled_clear();
  oled_update();
}

void oled_clear(void) { memset(framebuffer, 0, OLED_BUFFER_SIZE); }

// DMA page buffer (must persist until DMA transfer completes)
static uint8_t page_buf[129];
static uint8_t oled_dma_page = 4; // 4 = idle

void oled_update(void) {
  // Set column address range: 0 to 127
  oled_send_cmd(0x21);
  oled_send_cmd(0x00);
  oled_send_cmd(0x7F);
  // Set page address range: 0 to 3
  oled_send_cmd(0x22);
  oled_send_cmd(0x00);
  oled_send_cmd(0x03);

  // Start sending page 0 via DMA
  oled_dma_page = 0;
}

// Called from oled_display_task to progress DMA page transfers.
// Returns true when all pages have been sent.
bool oled_update_poll(void) {
  if (oled_dma_page >= 4)
    return true; // All done

  if (oled_i2c_dma_busy())
    return false; // Previous page still sending

  // Prepare and send current page
  page_buf[0] = 0x40; // Co=0, D/C#=1 (data)
  memcpy(&page_buf[1], &framebuffer[oled_dma_page * OLED_NATIVE_WIDTH],
         OLED_NATIVE_WIDTH);
  oled_i2c_write_dma(OLED_I2C_ADDR, page_buf, 129);
  oled_dma_page++;

  return false;
}

void oled_set_pixel(uint8_t x, uint8_t y, bool on) {
  if (x >= OLED_WIDTH || y >= OLED_HEIGHT)
    return;

  // Rotate 90 degrees: logical 32x128 -> native 128x32 framebuffer
  uint8_t nx = y;
  uint8_t ny = (uint8_t)(OLED_NATIVE_HEIGHT - 1 - x);

  uint16_t idx = (ny / 8) * OLED_NATIVE_WIDTH + nx;
  if (on)
    framebuffer[idx] |= (1 << (ny & 7));
  else
    framebuffer[idx] &= ~(1 << (ny & 7));
}

void oled_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on) {
  for (uint8_t dy = 0; dy < h; dy++) {
    for (uint8_t dx = 0; dx < w; dx++) {
      oled_set_pixel(x + dx, y + dy, on);
    }
  }
}

void oled_draw_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
  // Top and bottom edges
  for (uint8_t dx = 0; dx < w; dx++) {
    oled_set_pixel(x + dx, y, true);
    oled_set_pixel(x + dx, y + h - 1, true);
  }
  // Left and right edges
  for (uint8_t dy = 0; dy < h; dy++) {
    oled_set_pixel(x, y + dy, true);
    oled_set_pixel(x + w - 1, y + dy, true);
  }
}

void oled_draw_char_color(uint8_t x, uint8_t y, char c, bool on) {
  const uint8_t *glyph = font_glyph(c);

  for (uint8_t col = 0; col < 5; col++) {
    uint8_t bits = glyph[col];
    for (uint8_t row = 0; row < 7; row++) {
      if (bits & (1 << row))
        oled_set_pixel(x + col, y + row, on);
    }
  }
}

void oled_draw_char(uint8_t x, uint8_t y, char c) {
  oled_draw_char_color(x, y, c, true);
}

void oled_draw_string_color(uint8_t x, uint8_t y, const char *str, bool on) {
  while (*str) {
    oled_draw_char_color(x, y, *str, on);
    x += 6; // 5px char + 1px spacing
    str++;
  }
}

void oled_draw_string(uint8_t x, uint8_t y, const char *str) {
  oled_draw_string_color(x, y, str, true);
}

void oled_draw_number_color(uint8_t x, uint8_t y, uint16_t num, bool on) {
  char buf[6]; // max "65535" + null
  uint8_t pos = 5;

  buf[pos] = '\0';
  if (num == 0) {
    buf[--pos] = '0';
  } else {
    while (num > 0 && pos > 0) {
      buf[--pos] = '0' + (num % 10);
      num /= 10;
    }
  }

  oled_draw_string_color(x, y, &buf[pos], on);
}

void oled_draw_number(uint8_t x, uint8_t y, uint16_t num) {
  oled_draw_number_color(x, y, num, true);
}

#endif
