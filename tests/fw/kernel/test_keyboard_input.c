/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include "kernel/keyboard_input.h"

#include "clar.h"
#include "stubs_mutex.h"

#include <string.h>

// The real kernel queue holds MAX_KERNEL_EVENTS entries and blocks the caller for three seconds
// before resetting the watch, so a test that overruns it here would be a reboot on a real device.
#define MAX_KERNEL_EVENTS 32

static PebbleEvent s_events[MAX_KERNEL_EVENTS];
static size_t s_event_count;
static size_t s_queued_callbacks;
static CallbackEventCallback s_callback;

void event_put(PebbleEvent *event) {
  cl_assert(s_event_count < MAX_KERNEL_EVENTS);
  s_events[s_event_count++] = *event;
}

// Stands in for the PEBBLE_CALLBACK_EVENT the real helper puts on the kernel queue.
void launcher_task_add_callback(CallbackEventCallback callback, void *data) {
  cl_assert(s_queued_callbacks < MAX_KERNEL_EVENTS);
  ++s_queued_callbacks;
  s_callback = callback;
}

// Runs KernelMain's side of the hand-off, draining whatever the host task queued.
static void prv_deliver(void) {
  while (s_queued_callbacks) {
    --s_queued_callbacks;
    s_callback(NULL);
  }
}

static void prv_post(uint8_t first, uint8_t second) {
  const uint8_t report[8] = {0, 0, first, second, 0, 0, 0, 0};
  bt_keyboard_handle_report(report, sizeof(report));
}

static void prv_report(uint8_t first, uint8_t second) {
  prv_post(first, second);
  prv_deliver();
}

static void prv_handle(const uint8_t *report, size_t length) {
  bt_keyboard_handle_report(report, length);
  prv_deliver();
}

static void prv_release(void) {
  bt_keyboard_release_buttons();
  prv_deliver();
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
  prv_deliver();
  for (ButtonId id = 0; id < NUM_BUTTONS; ++id) {
    prv_filter(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, id);
    prv_filter(PEBBLE_BUTTON_UP_EVENT, id);
  }
  s_event_count = 0;
  s_queued_callbacks = 0;
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
  prv_handle(report, sizeof(report));
  cl_assert_equal_i(1, s_event_count);
  cl_assert_equal_i(BUTTON_ID_UP, s_events[0].button.button_id);
}

void test_keyboard_input__disconnect_releases_once(void) {
  prv_report(0x52, 0x28);
  prv_release();
  prv_release();
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
    prv_handle(good, length);
    cl_assert_equal_i(2, s_event_count);
    cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[1].type);
  }
  s_event_count = 0;
  prv_report(0x52, 0);
  prv_handle(NULL, 8);
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
  prv_handle(reserved, sizeof(reserved));
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

// Defect: prv_emit_buttons used to call event_put once per changed button straight from the
// Bluetooth host task. event_put blocks for three seconds on a full 32-entry kernel queue and then
// resets the watch, so a peer notifying at the minimum connection interval could reboot it.
void test_keyboard_input__report_flood_queues_one_callback_and_never_blocks(void) {
  for (unsigned i = 0; i < 1000; ++i) {
    prv_post(0x52, 0x28);
    prv_post(0x51, 0x29);
  }
  // One outstanding callback for the whole burst, against a queue that holds 32 entries.
  cl_assert_equal_i(1, s_queued_callbacks);
  cl_assert_equal_i(0, s_event_count);

  prv_deliver();
  // The burst ended on Back and Down held; Up and Select were tapped in between.
  cl_assert_equal_i(6, s_event_count);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[0].type);
  cl_assert_equal_i(BUTTON_ID_BACK, s_events[0].button.button_id);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[1].type);
  cl_assert_equal_i(BUTTON_ID_DOWN, s_events[1].button.button_id);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[2].type);
  cl_assert_equal_i(BUTTON_ID_UP, s_events[2].button.button_id);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[3].type);
  cl_assert_equal_i(BUTTON_ID_SELECT, s_events[3].button.button_id);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[4].type);
  cl_assert_equal_i(BUTTON_ID_UP, s_events[4].button.button_id);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[5].type);
  cl_assert_equal_i(BUTTON_ID_SELECT, s_events[5].button.button_id);

  s_event_count = 0;
  prv_report(0, 0);
  cl_assert_equal_i(2, s_event_count);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[0].type);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[1].type);
}

void test_keyboard_input__a_tap_between_deliveries_is_not_lost(void) {
  prv_post(0x52, 0);
  prv_post(0, 0);
  prv_deliver();
  cl_assert_equal_i(2, s_event_count);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[0].type);
  cl_assert_equal_i(BUTTON_ID_UP, s_events[0].button.button_id);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[1].type);
  cl_assert_equal_i(BUTTON_ID_UP, s_events[1].button.button_id);
}

void test_keyboard_input__a_tap_alongside_a_held_button_keeps_the_hold(void) {
  prv_report(0x52, 0);
  cl_assert_equal_i(1, s_event_count);
  prv_post(0x52, 0x28);
  prv_post(0x52, 0);
  prv_deliver();
  cl_assert_equal_i(3, s_event_count);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[1].type);
  cl_assert_equal_i(BUTTON_ID_SELECT, s_events[1].button.button_id);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[2].type);
  cl_assert_equal_i(BUTTON_ID_SELECT, s_events[2].button.button_id);
  // The physical hold survives the tap.
  prv_report(0, 0);
  cl_assert_equal_i(4, s_event_count);
  cl_assert_equal_i(BUTTON_ID_UP, s_events[3].button.button_id);
}

void test_keyboard_input__repeated_identical_reports_queue_nothing(void) {
  prv_report(0x52, 0);
  s_queued_callbacks = 0;
  for (unsigned i = 0; i < 100; ++i) {
    prv_post(0x52, 0);
  }
  cl_assert_equal_i(0, s_queued_callbacks);
}

void test_keyboard_input__a_release_between_deliveries_is_not_lost(void) {
  prv_report(0x52, 0);
  cl_assert_equal_i(1, s_event_count);
  // Released and pressed again before KernelMain runs: a plain before-and-after comparison sees an
  // unbroken hold, which a click recogniser would read as one long press instead of two.
  prv_post(0, 0);
  prv_post(0x52, 0);
  prv_deliver();
  cl_assert_equal_i(3, s_event_count);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[1].type);
  cl_assert_equal_i(BUTTON_ID_UP, s_events[1].button.button_id);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[2].type);
  cl_assert_equal_i(BUTTON_ID_UP, s_events[2].button.button_id);
  // Still held afterwards.
  prv_report(0, 0);
  cl_assert_equal_i(4, s_event_count);
  cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[3].type);
}

void test_keyboard_input__a_delivery_never_exceeds_the_from_kernel_budget(void) {
  // MAX_FROM_KERNEL_MAIN_EVENTS is 14; the worst case here is all four buttons tapped.
  prv_post(0x52, 0x28);
  prv_post(0x51, 0x29);
  prv_post(0, 0);
  prv_deliver();
  cl_assert_equal_i(8, s_event_count);
  for (size_t i = 0; i < 4; ++i) {
    cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT, s_events[i].type);
  }
  for (size_t i = 4; i < 8; ++i) {
    cl_assert_equal_i(PEBBLE_KEYBOARD_BUTTON_UP_EVENT, s_events[i].type);
  }
}
