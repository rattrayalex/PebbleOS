/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include "kernel/keyboard_input.h"

#include "clar.h"

#include <string.h>

static PebbleEvent s_events[32];
static size_t s_event_count;

void event_put(PebbleEvent *event) {
  cl_assert(s_event_count < 32);
  s_events[s_event_count++] = *event;
}

static void prv_report(uint8_t first, uint8_t second) {
  const uint8_t report[8] = {0, 0, first, second, 0, 0, 0, 0};
  bt_keyboard_handle_report(report, sizeof(report));
}

static bool prv_filter(PebbleEventType type, ButtonId id) {
  PebbleEvent event = {.type = type, .button.button_id = id};
  const bool accepted = keyboard_input_filter_event(&event);
  if (accepted && type == PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT) {
    cl_assert_equal_i(PEBBLE_BUTTON_DOWN_EVENT, event.type);
  } else if (accepted && type == PEBBLE_KEYBOARD_BUTTON_UP_EVENT) {
    cl_assert_equal_i(PEBBLE_BUTTON_UP_EVENT, event.type);
  }
  return accepted;
}

void test_keyboard_input__initialize(void) {
  bt_keyboard_release_buttons();
  for (ButtonId id = 0; id < NUM_BUTTONS; ++id) {
    prv_filter(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, id);
    prv_filter(PEBBLE_BUTTON_UP_EVENT, id);
  }
  s_event_count = 0;
}

void test_keyboard_input__navigation_mapping(void) {
  const uint8_t usages[] = {0x52, 0x51, 0x28, 0x4f, 0x29, 0x50, 0x2a};
  const ButtonId buttons[] = {BUTTON_ID_UP,   BUTTON_ID_DOWN, BUTTON_ID_SELECT, BUTTON_ID_SELECT,
                              BUTTON_ID_BACK, BUTTON_ID_BACK, BUTTON_ID_BACK};
  for (size_t i = 0; i < sizeof(usages); ++i) {
    s_event_count = 0;
    prv_report(usages[i], 0);
    prv_report(0, 0);
    cl_assert_equal_i(2, s_event_count);
    cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[0].type);
    cl_assert_equal_i(buttons[i], s_events[0].button.button_id);
    cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[1].type);
    cl_assert_equal_i(buttons[i], s_events[1].button.button_id);
  }
}

void test_keyboard_input__duplicate_reports_and_aliases_preserve_hold(void) {
  prv_report(0x28, 0x4f);
  prv_report(0x28, 0x4f);
  prv_report(0x4f, 0);
  cl_assert_equal_i(1, s_event_count);
  prv_report(0, 0);
  cl_assert_equal_i(2, s_event_count);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[1].type);
}

void test_keyboard_input__pairing_enter_is_ignored_until_released(void) {
  bt_keyboard_suppress_pairing_enter();
  prv_report(0x28, 0);
  prv_report(0x28, 0);
  prv_report(0, 0);
  cl_assert_equal_i(0, s_event_count);
  prv_report(0x28, 0);
  prv_report(0, 0);
  cl_assert_equal_i(2, s_event_count);
  cl_assert_equal_i(BUTTON_ID_SELECT, s_events[0].button.button_id);
}

void test_keyboard_input__pairing_enter_does_not_block_other_keys(void) {
  bt_keyboard_suppress_pairing_enter();
  prv_report(0x28, 0x52);
  cl_assert_equal_i(1, s_event_count);
  cl_assert_equal_i(BUTTON_ID_UP, s_events[0].button.button_id);
  prv_report(0x28, 0);
  cl_assert_equal_i(2, s_event_count);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[1].type);
  prv_report(0, 0);
  cl_assert_equal_i(2, s_event_count);
}

void test_keyboard_input__chords_release_before_press(void) {
  prv_report(0x52, 0x28);
  cl_assert_equal_i(2, s_event_count);
  prv_report(0x51, 0x29);
  cl_assert_equal_i(6, s_event_count);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[2].type);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[3].type);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[4].type);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[5].type);
}

void test_keyboard_input__all_six_slots_and_modifiers(void) {
  const uint8_t report[] = {0xff, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0x52};
  bt_keyboard_handle_report(report, sizeof(report));
  cl_assert_equal_i(1, s_event_count);
  cl_assert_equal_i(BUTTON_ID_UP, s_events[0].button.button_id);
}

void test_keyboard_input__disconnect_releases_once(void) {
  prv_report(0x52, 0x28);
  bt_keyboard_release_buttons();
  bt_keyboard_release_buttons();
  cl_assert_equal_i(4, s_event_count);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[2].type);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[3].type);
}

void test_keyboard_input__invalid_reports_release_held_buttons(void) {
  const uint8_t good[9] = {0, 0, 0x52};
  for (size_t length = 0; length <= sizeof(good); ++length) {
    if (length == 8) {
      continue;
    }
    s_event_count = 0;
    prv_report(0x52, 0);
    bt_keyboard_handle_report(good, length);
    cl_assert_equal_i(2, s_event_count);
    cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[1].type);
  }
  s_event_count = 0;
  prv_report(0x52, 0);
  bt_keyboard_handle_report(NULL, 8);
  cl_assert_equal_i(2, s_event_count);
}

void test_keyboard_input__rollover_and_reserved_byte_release(void) {
  for (uint8_t error = 1; error <= 3; ++error) {
    s_event_count = 0;
    prv_report(0x52, 0);
    prv_report(0x28, error);
    cl_assert_equal_i(2, s_event_count);
    cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[1].type);
  }
  s_event_count = 0;
  prv_report(0x52, 0);
  const uint8_t reserved[8] = {0, 1, 0x28};
  bt_keyboard_handle_report(reserved, sizeof(reserved));
  cl_assert_equal_i(2, s_event_count);
}

void test_keyboard_input__keyboard_release_preserves_physical_hold(void) {
  cl_assert(prv_filter(PEBBLE_BUTTON_DOWN_EVENT, BUTTON_ID_UP));
  cl_assert(!prv_filter(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, BUTTON_ID_UP));
  cl_assert(!prv_filter(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, BUTTON_ID_UP));
  cl_assert(prv_filter(PEBBLE_BUTTON_UP_EVENT, BUTTON_ID_UP));
}

void test_keyboard_input__physical_release_preserves_keyboard_hold(void) {
  cl_assert(prv_filter(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, BUTTON_ID_BACK));
  cl_assert(!prv_filter(PEBBLE_BUTTON_DOWN_EVENT, BUTTON_ID_BACK));
  cl_assert(!prv_filter(PEBBLE_BUTTON_UP_EVENT, BUTTON_ID_BACK));
  cl_assert(prv_filter(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, BUTTON_ID_BACK));
}

void test_keyboard_input__filter_ignores_other_events_and_invalid_ids(void) {
  PebbleEvent event = {.type = PEBBLE_BATTERY_STATE_CHANGE_EVENT};
  PebbleEvent original = event;
  cl_assert(keyboard_input_filter_event(&event));
  cl_assert_equal_m(&original, &event, sizeof(event));
  cl_assert(!prv_filter(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, NUM_BUTTONS));
  cl_assert(!prv_filter(PEBBLE_BUTTON_DOWN_EVENT, (ButtonId)-1));
}
