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
#include "wear_leveling.h"

//--------------------------------------------------------------------+
// Keyboard Persistent Configuration
//--------------------------------------------------------------------+

// Magic number to identify the start of the configuration
#define EECONFIG_MAGIC_START 0x0A42494C
// Magic number to identify the end of the configuration
#define EECONFIG_MAGIC_END 0x0A4B4D48

// Number of stored macros. Each macro is triggered by the keycode
// `SP_MACRO_MIN + index`, so this must not exceed the macro keycode range.
#define MACRO_COUNT 16
// Total size in bytes of the shared macro storage buffer. Macros are stored
// back-to-back as sequences of 2-byte events ([opcode, arg]) each terminated by
// a single `MACRO_OP_END` (0x00) byte. The buffer is global (shared across all
// profiles). See `macro.h` for the event encoding.
#define MACRO_BUFFER_SIZE 1024

// Keyboard calibration configuration
typedef struct __attribute__((packed)) {
  // Initial rest value of the key matrix. If the value is smaller than the
  // actual rest value, the key will have a dead zone at the beginning of the
  // keystroke. If the value is larger than the actual rest value, a longer
  // calibration process may be required.
  uint16_t initial_rest_value;
  // Minimum change in ADC values for the key to be considered bottom-out. If
  // the value is larger than the actual bottom-out threshold, the key will have
  // a dead zone at the end of the keystroke. If the value is smaller than the
  // actual bottom-out threshold, the distance calculation may be inaccurate
  // until the first bottom-out event.
  uint16_t initial_bottom_out_threshold;
  // Bottom-out dead zone in travel-distance counts (0-255). Once a key is
  // within this many counts of full press (distance >= 255 - this value), it is
  // treated as fully bottomed out (distance forced to 255). This absorbs ADC
  // saturation and stem wobble at the very end of the travel so that holding a
  // key hard against the bottom does not trip a false Rapid Trigger release
  // ("input drop-out"). It only affects the end of the travel, never the
  // actuation point or mid-travel Rapid Trigger. A value of 0 disables it.
  uint8_t bottom_out_deadzone;
} eeconfig_calibration_t;

// USB polling rate. Only applicable if USB HS is enabled; USB FS is always
// polled at 1kHz.
//
// For high-speed interrupt endpoints the descriptor's `bInterval` is an
// exponent: the polling period is 2^(bInterval - 1) microframes of 125us each
// (USB 2.0 section 9.6.6). The enumerators are therefore defined as
// `bInterval - 1` so that the descriptor value is simply `polling_rate + 1`.
//
// The default must stay 0 so that a zero-initialized configuration keeps the
// highest polling rate, matching the behavior before this field existed.
typedef enum {
  POLLING_RATE_8000HZ = 0,
  POLLING_RATE_4000HZ,
  POLLING_RATE_2000HZ,
  POLLING_RATE_1000HZ,
  POLLING_RATE_500HZ,
  POLLING_RATE_250HZ,
  POLLING_RATE_125HZ,

  POLLING_RATE_COUNT,
} polling_rate_t;

// Keyboard options configuration
typedef union __attribute__((packed)) {
  struct __attribute__((packed)) {
    // Whether the XInput interface is enabled
    bool xinput_enabled : 1;
    bool _unused0 : 1;
    // USB polling rate (`polling_rate_t`). Replaces the single-bit
    // `high_polling_rate_enabled` flag used up to configuration version 0x0108.
    uint16_t polling_rate : 3;
    // Reserved bits for future use
    uint16_t reserved : 11;
  };
  uint16_t raw;
} eeconfig_options_t;

_Static_assert(sizeof(eeconfig_options_t) == sizeof(uint16_t),
               "Invalid eeconfig_options_t size");

// Keyboard profile configuration
typedef struct __attribute__((packed)) {
  uint8_t keymap[NUM_LAYERS][NUM_KEYS];
  actuation_t actuation_map[NUM_KEYS];
  advanced_key_t advanced_keys[NUM_ADVANCED_KEYS];
  uint8_t gamepad_buttons[NUM_KEYS];
  gamepad_options_t gamepad_options;
  uint8_t tick_rate;
} eeconfig_profile_t;

// Persistent configuration version. The size of the configuration must be
// non-decreasing, so that the migration can assume that the new version is at
// least as large as the previous version.
#define EECONFIG_VERSION 0x0109

// Keyboard configuration
// Whenever there is a change in the configuration, `EECONFIG_VERSION` must be
// bumped. Make sure to update `eeconfig_reset()`, and add a migration function
// in `migration.c`.
typedef struct __attribute__((packed)) {
  // Global configurations
  // Magic number to identify the start of the configuration
  uint32_t magic_start;
  // Version of the configuration
  uint16_t version;

  // Calibration configuration
  eeconfig_calibration_t calibration;
  // Saved bottom-out threshold
  uint16_t bottom_out_threshold[NUM_KEYS];
  // Per-key switch type for calibration
  uint8_t switch_map[NUM_KEYS];
  // Options configuration
  eeconfig_options_t options;

  // Current profile index
  uint8_t current_profile;
  // Last non-default profile index, used for profile swapping
  uint8_t last_non_default_profile;
  // Shared macro storage buffer (global, not per-profile). See `macro.h`.
  uint8_t macros[MACRO_BUFFER_SIZE];
  // End of global configurations

  // Profiles
  eeconfig_profile_t profiles[NUM_PROFILES];

  // Magic number to identify the end of the configuration
  uint32_t magic_end;
} eeconfig_t;

_Static_assert(
    sizeof(eeconfig_t) <= WL_VIRTUAL_SIZE,
    "Keyboard configuration size must be at most the virtual storage size.");

extern const eeconfig_t *eeconfig;

#define CURRENT_PROFILE (eeconfig->profiles[eeconfig->current_profile])

//--------------------------------------------------------------------+
// Default Keyboard Configuration
//--------------------------------------------------------------------+

#if !defined(DEFAULT_CALIBRATION)
#error "DEFAULT_CALIBRATION is not defined"
#endif

#if !defined(DEFAULT_OPTIONS)
// Default global options
#define DEFAULT_OPTIONS                                                        \
  {                                                                            \
      .xinput_enabled = false,                                                 \
      .polling_rate = POLLING_RATE_8000HZ,                                     \
  }
#endif

#if !defined(DEFAULT_KEYMAPS)
#error "DEFAULT_KEYMAPS is not defined"
#endif

#if !defined(DEFAULT_ACTUATION_POINT)
// Default actuation point
#define DEFAULT_ACTUATION_POINT 128
#endif

#if !defined(DEFAULT_GAMEPAD_OPTIONS)
// Default gamepad options
#define DEFAULT_GAMEPAD_OPTIONS                                                \
  {                                                                            \
      .analog_curve = {{4, 20}, {85, 95}, {165, 170}, {255, 255}},             \
      .keyboard_enabled = true,                                                \
      .snappy_joystick = true,                                                 \
  }
#endif

#if !defined(DEFAULT_TICK_RATE)
// Default tick rate
#define DEFAULT_TICK_RATE 30
#endif

#if !defined(DEFAULT_BOTTOM_OUT_DEADZONE)
// Default bottom-out dead zone in travel-distance counts. Used by the migration
// path when an existing configuration is upgraded. For a fresh configuration
// the value comes from `DEFAULT_CALIBRATION` (keyboards/<board>/keyboard.json),
// so keep this in sync with that file's `calibration.bottom_out_deadzone`.
#define DEFAULT_BOTTOM_OUT_DEADZONE 10
#endif

//--------------------------------------------------------------------+
// Persistent Configuration API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the persistent configuration module
 *
 * @return None
 */
void eeconfig_init(void);

/**
 * @brief Reset the persistent configuration to default values
 *
 * @return true if successful, false otherwise
 */
bool eeconfig_reset(void);

/**
 * @brief Reset a specific profile to default values
 *
 * @param profile Profile index
 *
 * @return true if successful, false otherwise
 */
bool eeconfig_reset_profile(uint8_t profile);

/**
 * @brief Write a value to a field in the persistent configuration
 *
 * @param field Field to write to
 * @param value Value to write
 *
 * @return true if successful, false otherwise
 */
#define EECONFIG_WRITE(field, value)                                           \
  wear_leveling_write(offsetof(eeconfig_t, field), value,                      \
                      sizeof(((eeconfig_t *)0)->field))

/**
 * @brief Write a value to a field in the persistent configuration
 *
 * @param field Field to write to
 * @param value Value to write
 * @param len Length of the value in bytes
 *
 * @return true if successful, false otherwise
 */
#define EECONFIG_WRITE_N(field, value, len)                                    \
  wear_leveling_write(offsetof(eeconfig_t, field), value, len)
