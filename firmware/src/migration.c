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

#include "migration.h"

#include "eeconfig.h"
#include "wear_leveling.h"

static bool v1_1_global_config_func(uint8_t *dst, const uint8_t *src);
static bool v1_1_profile_config_func(uint8_t profile, uint8_t *dst,
                                     const uint8_t *src);

static bool v1_2_global_config_func(uint8_t *dst, const uint8_t *src);
static bool v1_2_profile_config_func(uint8_t profile, uint8_t *dst,
                                     const uint8_t *src);

static bool v1_3_global_config_func(uint8_t *dst, const uint8_t *src);
static bool v1_3_profile_config_func(uint8_t profile, uint8_t *dst,
                                     const uint8_t *src);

static bool v1_4_global_config_func(uint8_t *dst, const uint8_t *src);
static bool v1_4_profile_config_func(uint8_t profile, uint8_t *dst,
                                     const uint8_t *src);

static bool v1_6_global_config_func(uint8_t *dst, const uint8_t *src);
static bool v1_6_profile_config_func(uint8_t profile, uint8_t *dst,
                                     const uint8_t *src);

static bool v1_7_global_config_func(uint8_t *dst, const uint8_t *src);
static bool v1_7_profile_config_func(uint8_t profile, uint8_t *dst,
                                     const uint8_t *src);

static bool v1_8_global_config_func(uint8_t *dst, const uint8_t *src);
static bool v1_8_profile_config_func(uint8_t profile, uint8_t *dst,
                                     const uint8_t *src);

static bool v1_9_global_config_func(uint8_t *dst, const uint8_t *src);
static bool v1_9_profile_config_func(uint8_t profile, uint8_t *dst,
                                     const uint8_t *src);

// Migration metadata for each configuration version. The first entry is
// reserved for the initial version (v1.0) which does not require migration.
static const migration_t migrations[] = {
    {
        .version = 0x0100,
        .global_config_size = 12,
        .profile_config_size = NUM_LAYERS * NUM_KEYS    // Keymap
                               + NUM_KEYS * 4           // Actuation map
                               + NUM_ADVANCED_KEYS * 12 // Advanced keys
                               + 1                      // Tick rate
        ,
    },
    {
        .version = 0x0101,
        .global_config_size = 14,
        .profile_config_size = NUM_LAYERS * NUM_KEYS    // Keymap
                               + NUM_KEYS * 4           // Actuation map
                               + NUM_ADVANCED_KEYS * 12 // Advanced keys
                               + NUM_KEYS               // Gamepad buttons
                               + 9                      // Gamepad options
                               + 1                      // Tick rate
        ,
        .global_config_func = v1_1_global_config_func,
        .profile_config_func = v1_1_profile_config_func,
    },
    {
        .version = 0x0102,
        .global_config_size = 14             // Other fields
                              + NUM_KEYS * 2 // Bottom-out threshold
        ,
        .profile_config_size = NUM_LAYERS * NUM_KEYS    // Keymap
                               + NUM_KEYS * 4           // Actuation map
                               + NUM_ADVANCED_KEYS * 12 // Advanced keys
                               + NUM_KEYS               // Gamepad buttons
                               + 9                      // Gamepad options
                               + 1                      // Tick rate
        ,
        .global_config_func = v1_2_global_config_func,
        .profile_config_func = v1_2_profile_config_func,
    },
    {
        .version = 0x0103,
        .global_config_size = 14             // Other fields
                              + NUM_KEYS * 2 // Bottom-out threshold
        ,
        .profile_config_size = NUM_LAYERS * NUM_KEYS    // Keymap
                               + NUM_KEYS * 4           // Actuation map
                               + NUM_ADVANCED_KEYS * 12 // Advanced keys
                               + NUM_KEYS               // Gamepad buttons
                               + 9                      // Gamepad options
                               + 1                      // Tick rate
        ,
        .global_config_func = v1_3_global_config_func,
        .profile_config_func = v1_3_profile_config_func,
    },
    {
        .version = 0x0104,
        .global_config_size = 14             // Other fields
                              + NUM_KEYS * 2 // Bottom-out threshold
        ,
        .profile_config_size = NUM_LAYERS * NUM_KEYS    // Keymap
                               + NUM_KEYS * 4           // Actuation map
                               + NUM_ADVANCED_KEYS * 12 // Advanced keys
                               + NUM_KEYS               // Gamepad buttons
                               + 9                      // Gamepad options
                               + 1                      // Tick rate
        ,
        .global_config_func = v1_4_global_config_func,
        .profile_config_func = v1_4_profile_config_func,
    },
    {
        .version = 0x0106,
        .global_config_size = 14             // Other fields
                              + NUM_KEYS * 2 // Bottom-out threshold
                              + NUM_KEYS     // Switch map
        ,
        .profile_config_size = NUM_LAYERS * NUM_KEYS    // Keymap
                               + NUM_KEYS * 4           // Actuation map
                               + NUM_ADVANCED_KEYS * 12 // Advanced keys
                               + NUM_KEYS               // Gamepad buttons
                               + 9                      // Gamepad options
                               + 1                      // Tick rate
        ,
        .global_config_func = v1_6_global_config_func,
        .profile_config_func = v1_6_profile_config_func,
    },
    {
        .version = 0x0107,
        .global_config_size = 14             // Other fields
                              + 1            // Bottom-out dead zone
                              + NUM_KEYS * 2 // Bottom-out threshold
                              + NUM_KEYS     // Switch map
        ,
        .profile_config_size = NUM_LAYERS * NUM_KEYS    // Keymap
                               + NUM_KEYS * 4           // Actuation map
                               + NUM_ADVANCED_KEYS * 12 // Advanced keys
                               + NUM_KEYS               // Gamepad buttons
                               + 9                      // Gamepad options
                               + 1                      // Tick rate
        ,
        .global_config_func = v1_7_global_config_func,
        .profile_config_func = v1_7_profile_config_func,
    },
    {
        .version = 0x0108,
        .global_config_size = 14                  // Other fields
                              + 1                 // Bottom-out dead zone
                              + NUM_KEYS * 2      // Bottom-out threshold
                              + NUM_KEYS          // Switch map
                              + MACRO_BUFFER_SIZE // Macro buffer
        ,
        .profile_config_size = NUM_LAYERS * NUM_KEYS    // Keymap
                               + NUM_KEYS * 4           // Actuation map
                               + NUM_ADVANCED_KEYS * 12 // Advanced keys
                               + NUM_KEYS               // Gamepad buttons
                               + 9                      // Gamepad options
                               + 1                      // Tick rate
        ,
        .global_config_func = v1_8_global_config_func,
        .profile_config_func = v1_8_profile_config_func,
    },
    {
        .version = 0x0109,
        .global_config_size = 14                  // Other fields
                              + 1                 // Bottom-out dead zone
                              + NUM_KEYS * 2      // Bottom-out threshold
                              + NUM_KEYS          // Switch map
                              + MACRO_BUFFER_SIZE // Macro buffer
        ,
        .profile_config_size = NUM_LAYERS * NUM_KEYS    // Keymap
                               + NUM_KEYS * 4           // Actuation map
                               + NUM_ADVANCED_KEYS * 12 // Advanced keys
                               + NUM_KEYS               // Gamepad buttons
                               + 9                      // Gamepad options
                               + 1                      // Tick rate
        ,
        .global_config_func = v1_9_global_config_func,
        .profile_config_func = v1_9_profile_config_func,
    },
};

bool migration_try_migrate(void) {
  if (eeconfig->magic_start != EECONFIG_MAGIC_START)
    // The magic start is always the same for any version.
    return false;

  const uint16_t config_version = eeconfig->version;
  // We alternate between two buffers to save the memory.
  uint8_t current_buf = 0;
  uint8_t bufs[2][sizeof(eeconfig_t)];

  // Let `bufs[0]` be the current configuration.
  memcpy(bufs[0], eeconfig, sizeof(eeconfig_t));
  // Skip v1.0 migration since it is the initial version
  for (uint32_t i = 1; i < M_ARRAY_SIZE(migrations); i++) {
    const migration_t *m = &migrations[i];
    const migration_t *prev_m = &migrations[i - 1];

    if (m->version <= config_version)
      // Skip migrations that are not applicable
      continue;

    const uint8_t *src = bufs[current_buf];
    uint8_t *dst = bufs[current_buf ^ 1];

    if (m->global_config_func && !m->global_config_func(dst, src))
      // Migration failed for the global configuration
      return false;

    if (m->profile_config_func) {
      for (uint8_t p = 0; p < NUM_PROFILES; p++) {
        // Move the pointers to the start of each profile configuration
        const uint8_t *profile_src =
            src + prev_m->global_config_size + p * prev_m->profile_config_size;
        uint8_t *profile_dst =
            dst + m->global_config_size + p * m->profile_config_size;

        if (!m->profile_config_func(p, profile_dst, profile_src))
          // Migration failed for the profile configuration
          return false;
      }
    }

    // Update the version in the destination buffer
    ((eeconfig_t *)dst)->version = m->version;
    // Switch to the next buffer for the next migration
    current_buf ^= 1;
  }

  // Make sure the configuration is valid after migration
  ((eeconfig_t *)bufs[current_buf])->magic_end = EECONFIG_MAGIC_END;
  // We reflect the update in the flash.
  return wear_leveling_write(0, &bufs[current_buf], sizeof(eeconfig_t));
}

//--------------------------------------------------------------------+
// Helper Functions
//--------------------------------------------------------------------+

static void migration_memcpy(uint8_t **dst, const uint8_t **src, uint32_t len) {
  memcpy(*dst, *src, len);
  *dst += len;
  *src += len;
}

static void migration_memset(uint8_t **dst, uint8_t value, uint32_t len) {
  memset(*dst, value, len);
  *dst += len;
}

#define MAKE_MIGRATION_ASSIGN(type)                                            \
  static void migration_assign_##type(uint8_t **dst, type value) {             \
    *(type *)(*dst) = value;                                                   \
    *dst = (uint8_t *)((type *)(*dst) + 1);                                    \
  }

MAKE_MIGRATION_ASSIGN(uint8_t)
MAKE_MIGRATION_ASSIGN(uint16_t)

//--------------------------------------------------------------------+
// v1.0 -> v1.1 Migration
//--------------------------------------------------------------------+

bool v1_1_global_config_func(uint8_t *dst, const uint8_t *src) {
  if (((eeconfig_t *)src)->version != 0x0100)
    // Expected version v1.0
    return false;

  // Copy `magic_start` to `calibration`
  migration_memcpy(&dst, &src, 10);
  // Default `options` to 0
  migration_assign_uint16_t(&dst, 0);
  // Copy `current_profile` and `last_non_default_profile`
  migration_memcpy(&dst, &src, 2);

  return true;
}

bool v1_1_profile_config_func(uint8_t profile, uint8_t *dst,
                              const uint8_t *src) {
  // Save the `keymap` offset
  uint8_t *keymap = dst;
  // Copy `keymap` to `actuation_map`
  migration_memcpy(&dst, &src, (NUM_LAYERS * NUM_KEYS) + (NUM_KEYS * 4));
  // Update keycodes to include `KC_INT1` ... `KC_LNG6`
  for (uint32_t i = 0; i < NUM_LAYERS * NUM_KEYS; i++) {
    if (0x70 <= keymap[i] && keymap[i] <= 0x71)
      // `KC_LNG1` and `KC_LNG2`
      keymap[i] += 0x06;
    else if (0x72 <= keymap[i] && keymap[i] <= 0x96)
      // `KC_LEFT_CTRL` ... `SP_MOUSE_BUTTON_5`
      keymap[i] += 0x09;
  }
  // Save the `advanced_keys` offset
  uint8_t *advanced_keys = dst;
  // Copy `advanced_keys`
  migration_memcpy(&dst, &src, NUM_ADVANCED_KEYS * 12);
  // Default `hold_on_other_key_press` to 0
  for (uint8_t i = 0; i < NUM_ADVANCED_KEYS; i++) {
    uint8_t *ak = advanced_keys + i * 12;
    if (ak[2] == AK_TYPE_TAP_HOLD)
      ak[7] = 0;
  }
  // Set `gamepad_buttons` to 0
  migration_memset(&dst, 0, NUM_KEYS);
  // Default `analog_curve` to linear
  migration_assign_uint8_t(&dst, 4), migration_assign_uint8_t(&dst, 20);
  migration_assign_uint8_t(&dst, 85), migration_assign_uint8_t(&dst, 95);
  migration_assign_uint8_t(&dst, 165), migration_assign_uint8_t(&dst, 170);
  migration_assign_uint8_t(&dst, 255), migration_assign_uint8_t(&dst, 255);
  // Default `keyboard_enabled` and `snappy_joystick` to true
  migration_assign_uint8_t(&dst, 0b00001001);
  // Copy `tick_rate`
  migration_memcpy(&dst, &src, 1);

  return true;
}

//--------------------------------------------------------------------+
// v1.1 -> v1.2 Migration
//--------------------------------------------------------------------+

bool v1_2_global_config_func(uint8_t *dst, const uint8_t *src) {
  if (((eeconfig_t *)src)->version != 0x0101)
    // Expected version v1.1
    return false;

  // Copy `magic_start` to `calibration`
  migration_memcpy(&dst, &src, 10);
  // Set `bottom_out_threshold` to 0
  migration_memset(&dst, 0, NUM_KEYS * 2);
  // Copy `options` to `last_non_default_profile`
  migration_memcpy(&dst, &src, 4);

  return true;
}

bool v1_2_profile_config_func(uint8_t profile, uint8_t *dst,
                              const uint8_t *src) {
  // Copy the entire profile
  migration_memcpy(&dst, &src,
                   NUM_LAYERS * NUM_KEYS + NUM_KEYS * 4 +
                       NUM_ADVANCED_KEYS * 12 + NUM_KEYS + 9 + 1);

  return true;
}

//--------------------------------------------------------------------+
// v1.2 -> v1.3 Migration
//--------------------------------------------------------------------+

bool v1_3_global_config_func(uint8_t *dst, const uint8_t *src) {
  if (((eeconfig_t *)src)->version != 0x0102)
    // Expected version v1.2
    return false;

  // Copy `magic_start` to `bottom_out_threshold`
  migration_memcpy(&dst, &src, 10 + NUM_KEYS * 2);
  // Default `save_bottom_out_threshold` to true
  uint16_t options = *((uint16_t *)src) | (1 << 1);
  migration_assign_uint16_t(&dst, options);
  src += sizeof(options);
  // Copy `current_profile` to `last_non_default_profile`
  migration_memcpy(&dst, &src, 2);

  return true;
}

bool v1_3_profile_config_func(uint8_t profile, uint8_t *dst,
                              const uint8_t *src) {
  // Copy the entire profile
  migration_memcpy(&dst, &src,
                   NUM_LAYERS * NUM_KEYS + NUM_KEYS * 4 +
                       NUM_ADVANCED_KEYS * 12 + NUM_KEYS + 9 + 1);

  return true;
}

//--------------------------------------------------------------------+
// v1.3 -> v1.4 Migration
//--------------------------------------------------------------------+

bool v1_4_global_config_func(uint8_t *dst, const uint8_t *src) {
  if (((eeconfig_t *)src)->version != 0x0103)
    // Expected version v1.3
    return false;

  // Copy `magic_start` to `bottom_out_threshold`
  migration_memcpy(&dst, &src, 10 + NUM_KEYS * 2);
  // Default `high_polling_rate_enabled` to true
  uint16_t options = *((uint16_t *)src) | (1 << 2);
  migration_assign_uint16_t(&dst, options);
  src += sizeof(options);
  // Copy `current_profile` to `last_non_default_profile`
  migration_memcpy(&dst, &src, 2);

  return true;
}

bool v1_4_profile_config_func(uint8_t profile, uint8_t *dst,
                              const uint8_t *src) {
  // Copy the entire profile
  migration_memcpy(&dst, &src,
                   NUM_LAYERS * NUM_KEYS + NUM_KEYS * 4 +
                       NUM_ADVANCED_KEYS * 12 + NUM_KEYS + 9 + 1);

  return true;
}

//--------------------------------------------------------------------+
// v1.4/v1.5 -> v1.6 Migration (add per-key switch_map)
//--------------------------------------------------------------------+

bool v1_6_global_config_func(uint8_t *dst, const uint8_t *src) {
  uint16_t ver = ((eeconfig_t *)src)->version;
  if (ver != 0x0104 && ver != 0x0105)
    return false;

  // Copy `magic_start` to `bottom_out_threshold`
  migration_memcpy(&dst, &src, 10 + NUM_KEYS * 2);
  // Initialize `switch_map` to SWITCH_TYPE_AUTO (0)
  migration_memset(&dst, 0, NUM_KEYS);
  // Copy `options` to `last_non_default_profile`
  migration_memcpy(&dst, &src, 4);

  return true;
}

bool v1_6_profile_config_func(uint8_t profile, uint8_t *dst,
                              const uint8_t *src) {
  // Copy the entire profile (unchanged)
  migration_memcpy(&dst, &src,
                   NUM_LAYERS * NUM_KEYS + NUM_KEYS * 4 +
                       NUM_ADVANCED_KEYS * 12 + NUM_KEYS + 9 + 1);

  return true;
}

//--------------------------------------------------------------------+
// v1.6 -> v1.7 Migration (add calibration.bottom_out_deadzone)
//--------------------------------------------------------------------+

bool v1_7_global_config_func(uint8_t *dst, const uint8_t *src) {
  if (((eeconfig_t *)src)->version != 0x0106)
    // Expected version v1.6
    return false;

  // Copy `magic_start` through `calibration.initial_bottom_out_threshold`
  migration_memcpy(&dst, &src, 10);
  // Initialize the new `calibration.bottom_out_deadzone`
  migration_assign_uint8_t(&dst, DEFAULT_BOTTOM_OUT_DEADZONE);
  // Copy `bottom_out_threshold` through `last_non_default_profile`
  migration_memcpy(&dst, &src, NUM_KEYS * 2 + NUM_KEYS + 4);

  return true;
}

bool v1_7_profile_config_func(uint8_t profile, uint8_t *dst,
                              const uint8_t *src) {
  // Copy the entire profile (unchanged)
  migration_memcpy(&dst, &src,
                   NUM_LAYERS * NUM_KEYS + NUM_KEYS * 4 +
                       NUM_ADVANCED_KEYS * 12 + NUM_KEYS + 9 + 1);

  return true;
}

//--------------------------------------------------------------------+
// v1.7 -> v1.8 Migration (add global macro buffer)
//--------------------------------------------------------------------+

bool v1_8_global_config_func(uint8_t *dst, const uint8_t *src) {
  if (((eeconfig_t *)src)->version != 0x0107)
    // Expected version v1.7
    return false;

  // Copy `magic_start` through `last_non_default_profile` (the entire old
  // global block: 15 fixed bytes + bottom-out threshold + switch map).
  migration_memcpy(&dst, &src, 15 + NUM_KEYS * 2 + NUM_KEYS);
  // Initialize the new macro buffer to 0 (all macros empty).
  migration_memset(&dst, 0, MACRO_BUFFER_SIZE);

  return true;
}

bool v1_8_profile_config_func(uint8_t profile, uint8_t *dst,
                              const uint8_t *src) {
  // Copy the entire profile (unchanged)
  migration_memcpy(&dst, &src,
                   NUM_LAYERS * NUM_KEYS + NUM_KEYS * 4 +
                       NUM_ADVANCED_KEYS * 12 + NUM_KEYS + 9 + 1);

  return true;
}

//--------------------------------------------------------------------+
// v1.8 -> v1.9 Migration (polling rate flag -> 3-bit field)
//--------------------------------------------------------------------+

bool v1_9_global_config_func(uint8_t *dst, const uint8_t *src) {
  if (((eeconfig_t *)src)->version != 0x0108)
    // Expected version v1.8
    return false;

  // Copy `magic_start` through `switch_map` (11 fixed bytes + bottom-out
  // threshold + switch map).
  migration_memcpy(&dst, &src, 11 + NUM_KEYS * 2 + NUM_KEYS);
  // Bit 2 of `options` used to be the single-bit `high_polling_rate_enabled`
  // and is now the low bit of the 3-bit `polling_rate` field. Map the old flag
  // onto the matching enumerator and clear the previously reserved bits.
  uint16_t options;
  memcpy(&options, src, sizeof(options));
  src += sizeof(options);
  const uint16_t polling_rate = (options & 0x0004u)
                                    ? (uint16_t)POLLING_RATE_8000HZ
                                    : (uint16_t)POLLING_RATE_1000HZ;
  migration_assign_uint16_t(
      &dst, (uint16_t)((options & 0x0003u) | (uint16_t)(polling_rate << 2)));
  // Copy `current_profile` through `macros`
  migration_memcpy(&dst, &src, 2 + MACRO_BUFFER_SIZE);

  return true;
}

bool v1_9_profile_config_func(uint8_t profile, uint8_t *dst,
                              const uint8_t *src) {
  // Copy the entire profile (unchanged)
  migration_memcpy(&dst, &src,
                   NUM_LAYERS * NUM_KEYS + NUM_KEYS * 4 +
                       NUM_ADVANCED_KEYS * 12 + NUM_KEYS + 9 + 1);

  return true;
}
