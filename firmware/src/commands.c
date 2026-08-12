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

#include "commands.h"

#include "advanced_keys.h"
#include "hardware/hardware.h"
#include "iap.h"
#include "layout.h"
#include "matrix.h"
#include "metadata.h"
#include "tusb.h"

// Helper macro to verify command parameters
#define COMMAND_VERIFY(cond)                                                   \
  if (!(cond)) {                                                               \
    success = false;                                                           \
    break;                                                                     \
  }

static uint8_t out_buf[RAW_HID_EP_SIZE];
static const uint8_t keyboard_metadata[] = {KEYBOARD_METADATA};

void command_init(void) {}

void command_process(const uint8_t *buf) {
  const command_in_buffer_t *in = (const command_in_buffer_t *)buf;
  command_out_buffer_t *out = (command_out_buffer_t *)out_buf;

  bool success = true;
  switch (in->command_id) {
  case COMMAND_FIRMWARE_VERSION: {
    out->firmware_version = FIRMWARE_VERSION;
    break;
  }
  case COMMAND_REBOOT: {
    board_reset();
    break;
  }
  case COMMAND_BOOTLOADER: {
    board_enter_bootloader();
    break;
  }
  case COMMAND_FACTORY_RESET: {
    advanced_key_clear();
    success = eeconfig_reset();
    layout_load_advanced_keys();
    break;
  }
  case COMMAND_RECALIBRATE: {
    matrix_recalibrate(true);
    break;
  }
  case COMMAND_ANALOG_INFO: {
    const command_in_analog_info_t *p = &in->analog_info;
    command_out_analog_info_t *o = out->analog_info;

    COMMAND_VERIFY(p->offset < NUM_KEYS);

    for (uint32_t i = 0;
         i < M_ARRAY_SIZE(out->analog_info) && i + p->offset < NUM_KEYS; i++) {
      o[i].adc_value = key_matrix[i + p->offset].adc_filtered;
      o[i].distance = key_matrix[i + p->offset].distance;
    }
    break;
  }
  case COMMAND_GET_CALIBRATION: {
    out->calibration = eeconfig->calibration;
    break;
  }
  case COMMAND_SET_CALIBRATION: {
    success = EECONFIG_WRITE(calibration, &in->calibration);
    break;
  }
  case COMMAND_GET_PROFILE: {
    out->current_profile = eeconfig->current_profile;
    break;
  }
  case COMMAND_GET_OPTIONS: {
    out->options = eeconfig->options;
    break;
  }
  case COMMAND_SET_OPTIONS: {
    success = EECONFIG_WRITE(options, &in->options);
    break;
  }
  case COMMAND_RESET_PROFILE: {
    const command_in_reset_profile_t *p = &in->reset_profile;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    if (p->profile == eeconfig->current_profile)
      advanced_key_clear();
    success = eeconfig_reset_profile(p->profile);
    if (p->profile == eeconfig->current_profile)
      layout_load_advanced_keys();
    break;
  }
  case COMMAND_DUPLICATE_PROFILE: {
    const command_in_duplicate_profile_t *p = &in->duplicate_profile;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->src_profile < NUM_PROFILES);

    if (p->profile == eeconfig->current_profile)
      advanced_key_clear();
    success = EECONFIG_WRITE(profiles[p->profile],
                             &eeconfig->profiles[p->src_profile]);
    if (p->profile == eeconfig->current_profile)
      layout_load_advanced_keys();
    break;
  }
  case COMMAND_GET_KEYMAP: {
    const command_in_keymap_t *p = &in->keymap;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->layer < NUM_LAYERS);
    COMMAND_VERIFY(p->offset < NUM_KEYS);

    memcpy(out->keymap,
           eeconfig->profiles[p->profile].keymap[p->layer] + p->offset,
           M_MIN(M_ARRAY_SIZE(out->keymap), (uint32_t)(NUM_KEYS - p->offset)) *
               sizeof(uint8_t));
    break;
  }
  case COMMAND_GET_METADATA: {
    const command_in_metadata_t *p = &in->metadata;

    COMMAND_VERIFY(p->offset < sizeof(keyboard_metadata));

    out->metadata.len = sizeof(keyboard_metadata) - p->offset;
    memcpy(out->metadata.metadata, &keyboard_metadata[p->offset],
           M_MIN(sizeof(out->metadata.metadata), out->metadata.len));
    break;
  }
  case COMMAND_GET_SERIAL: {
    memset(out->serial, 0, sizeof(out->serial));
    board_serial(out->serial);
    break;
  }
  case COMMAND_SAVE_CALIBRATION_THRESHOLD: {
    uint16_t bottom_out_threshold[NUM_KEYS];

    for (uint32_t i = 0; i < NUM_KEYS; i++) {
      if (key_matrix[i].adc_bottom_out_value < key_matrix[i].adc_rest_value)
        bottom_out_threshold[i] = 0;
      else
        bottom_out_threshold[i] =
            key_matrix[i].adc_bottom_out_value - key_matrix[i].adc_rest_value;
    }
    success = EECONFIG_WRITE(bottom_out_threshold, bottom_out_threshold);
    break;
  }
  case COMMAND_GET_SWITCH_MAP: {
    const command_in_get_switch_map_t *p = &in->get_switch_map;

    COMMAND_VERIFY(p->offset < NUM_KEYS);

    memcpy(out->switch_map, eeconfig->switch_map + p->offset,
           M_MIN(M_ARRAY_SIZE(out->switch_map),
                 (uint32_t)(NUM_KEYS - p->offset)) *
               sizeof(uint8_t));
    break;
  }
  case COMMAND_SET_SWITCH_MAP: {
    const command_in_set_switch_map_t *p = &in->set_switch_map;

    COMMAND_VERIFY(p->offset < NUM_KEYS);
    COMMAND_VERIFY(p->len <= M_ARRAY_SIZE(p->switch_map) &&
                   p->len <= NUM_KEYS - p->offset);

    success = EECONFIG_WRITE_N(switch_map[p->offset], p->switch_map,
                               sizeof(uint8_t) * p->len);
    break;
  }
  case COMMAND_GET_MACRO: {
    const command_in_get_macro_t *p = &in->get_macro;

    COMMAND_VERIFY(p->offset < MACRO_BUFFER_SIZE);

    memcpy(out->macro, eeconfig->macros + p->offset,
           M_MIN(M_ARRAY_SIZE(out->macro),
                 (uint32_t)(MACRO_BUFFER_SIZE - p->offset)) *
               sizeof(uint8_t));
    break;
  }
  case COMMAND_SET_MACRO: {
    const command_in_set_macro_t *p = &in->set_macro;

    COMMAND_VERIFY(p->offset < MACRO_BUFFER_SIZE);
    COMMAND_VERIFY(p->len <= M_ARRAY_SIZE(p->data) &&
                   p->len <= MACRO_BUFFER_SIZE - p->offset);

    success = EECONFIG_WRITE_N(macros[p->offset], p->data,
                               sizeof(uint8_t) * p->len);
    break;
  }
    //--------------------------------------------------------------------+
    // In-app firmware update (IAP) commands
    //
    // These always echo the command ID back; errors are reported via the
    // status byte in the response payload instead of `COMMAND_UNKNOWN`.
    //--------------------------------------------------------------------+
  case COMMAND_FW_UPDATE_INIT: {
    const command_in_fw_update_init_t *p = &in->fw_update_init;
    command_out_fw_update_init_t *o = &out->fw_update_init;

    o->status = iap_init(p->size, p->crc32);
    o->chunk_size = IAP_CHUNK_SIZE;
    o->firmware_version = FIRMWARE_VERSION;
    o->staging_size = IAP_STAGING_SIZE;
    o->app_max_size = IAP_APP_MAX_SIZE;
    break;
  }
  case COMMAND_FW_UPDATE_WRITE: {
    const command_in_fw_update_write_t *p = &in->fw_update_write;
    command_out_fw_update_write_t *o = &out->fw_update_write;

    uint32_t next_offset = 0;
    o->status = iap_write(p->offset, p->data, p->len, &next_offset);
    o->next_offset = next_offset;
    break;
  }
  case COMMAND_FW_UPDATE_VERIFY: {
    command_out_fw_update_verify_t *o = &out->fw_update_verify;

    uint32_t computed_crc32 = 0;
    o->status = iap_verify(&computed_crc32);
    o->crc32 = computed_crc32;
    break;
  }
  case COMMAND_FW_UPDATE_APPLY: {
    const command_in_fw_update_apply_t *p = &in->fw_update_apply;
    command_out_fw_update_apply_t *o = &out->fw_update_apply;

    // The actual flash operation runs from `iap_task` after a short delay so
    // that this response can reach the host first. The device then resets.
    o->status = iap_apply_request(p->magic);
    break;
  }
    //--------------------------------------------------------------------+
    // Per-profile commands
    //--------------------------------------------------------------------+
  case COMMAND_SET_KEYMAP: {
    const command_in_keymap_t *p = &in->keymap;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->layer < NUM_LAYERS);
    COMMAND_VERIFY(p->offset < NUM_KEYS);
    COMMAND_VERIFY(p->len <= M_ARRAY_SIZE(p->keymap) &&
                   p->len <= NUM_KEYS - p->offset);

    success = EECONFIG_WRITE_N(profiles[p->profile].keymap[p->layer][p->offset],
                               p->keymap, sizeof(uint8_t) * p->len);
    break;
  }
  case COMMAND_GET_ACTUATION_MAP: {
    const command_in_actuation_map_t *p = &in->actuation_map;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_KEYS);

    memcpy(out->actuation_map,
           eeconfig->profiles[p->profile].actuation_map + p->offset,
           M_MIN(M_ARRAY_SIZE(out->actuation_map),
                 (uint32_t)(NUM_KEYS - p->offset)) *
               sizeof(actuation_t));
    break;
  }
  case COMMAND_SET_ACTUATION_MAP: {
    const command_in_actuation_map_t *p = &in->actuation_map;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_KEYS);
    COMMAND_VERIFY(p->len <= M_ARRAY_SIZE(p->actuation_map) &&
                   p->len <= NUM_KEYS - p->offset);

    success = EECONFIG_WRITE_N(profiles[p->profile].actuation_map[p->offset],
                               p->actuation_map, sizeof(actuation_t) * p->len);
    break;
  }
  case COMMAND_GET_ADVANCED_KEYS: {
    const command_in_advanced_keys_t *p = &in->advanced_keys;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_ADVANCED_KEYS);

    memcpy(out->advanced_keys,
           eeconfig->profiles[p->profile].advanced_keys + p->offset,
           M_MIN(M_ARRAY_SIZE(out->advanced_keys),
                 (uint32_t)(NUM_ADVANCED_KEYS - p->offset)) *
               sizeof(advanced_key_t));
    break;
  }
  case COMMAND_SET_ADVANCED_KEYS: {
    const command_in_advanced_keys_t *p = &in->advanced_keys;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_ADVANCED_KEYS);
    COMMAND_VERIFY(p->len <= M_ARRAY_SIZE(p->advanced_keys) &&
                   p->len <= NUM_ADVANCED_KEYS - p->offset);

    if (p->profile == eeconfig->current_profile)
      advanced_key_clear();
    success =
        EECONFIG_WRITE_N(profiles[p->profile].advanced_keys[p->offset],
                         p->advanced_keys, sizeof(advanced_key_t) * p->len);
    if (p->profile == eeconfig->current_profile)
      layout_load_advanced_keys();
    break;
  }
  case COMMAND_GET_TICK_RATE: {
    const command_in_tick_rate_t *p = &in->tick_rate;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    out->tick_rate = eeconfig->profiles[p->profile].tick_rate;
    break;
  }
  case COMMAND_SET_TICK_RATE: {
    const command_in_tick_rate_t *p = &in->tick_rate;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    success = EECONFIG_WRITE(profiles[p->profile].tick_rate, &p->tick_rate);
    break;
  }
  case COMMAND_GET_GAMEPAD_BUTTONS: {
    const command_in_gamepad_buttons_t *p = &in->gamepad_buttons;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_KEYS);

    memcpy(out->gamepad_buttons,
           eeconfig->profiles[p->profile].gamepad_buttons + p->offset,
           M_MIN(M_ARRAY_SIZE(out->gamepad_buttons),
                 (uint32_t)(NUM_KEYS - p->offset)) *
               sizeof(uint8_t));
    break;
  }
  case COMMAND_SET_GAMEPAD_BUTTONS: {
    const command_in_gamepad_buttons_t *p = &in->gamepad_buttons;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_KEYS);
    COMMAND_VERIFY(p->len <= M_ARRAY_SIZE(p->gamepad_buttons) &&
                   p->len <= NUM_KEYS - p->offset);

    success = EECONFIG_WRITE_N(profiles[p->profile].gamepad_buttons[p->offset],
                               p->gamepad_buttons, sizeof(uint8_t) * p->len);
    break;
  }
  case COMMAND_GET_GAMEPAD_OPTIONS: {
    const command_in_gamepad_options_t *p = &in->gamepad_options;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    out->gamepad_options = eeconfig->profiles[p->profile].gamepad_options;
    break;
  }
  case COMMAND_SET_GAMEPAD_OPTIONS: {
    const command_in_gamepad_options_t *p = &in->gamepad_options;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    success = EECONFIG_WRITE(profiles[p->profile].gamepad_options,
                             &p->gamepad_options);
    break;
  }
  default: {
    // Unknown command
    success = false;
    break;
  }
  }

  // Echo the command ID back to the host if successful
  out->command_id = success ? in->command_id : COMMAND_UNKNOWN;

  while (!tud_hid_n_ready(USB_ITF_RAW_HID))
    // Wait for the raw HID interface to be ready
    tud_task();
  tud_hid_n_report(USB_ITF_RAW_HID, 0, out_buf, RAW_HID_EP_SIZE);
}
