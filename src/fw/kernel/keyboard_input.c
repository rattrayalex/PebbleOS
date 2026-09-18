/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include "keyboard_input.h"

#ifdef CONFIG_BT_KEYBOARD

// The Bluetooth host owns the report state; KernelMain owns the two input sources.
static uint8_t s_report_buttons;
static uint8_t s_keyboard_buttons;
static uint8_t s_other_buttons;
static bool s_suppress_enter;

static uint8_t prv_button_for_usage(uint8_t usage) {
  switch (usage) {
    case 0x52: // Up arrow
      return 1u << BUTTON_ID_UP;
    case 0x51: // Down arrow
      return 1u << BUTTON_ID_DOWN;
    case 0x28: // Enter
    case 0x4f: // Right arrow
      return 1u << BUTTON_ID_SELECT;
    case 0x29: // Escape
    case 0x50: // Left arrow
    case 0x2a: // Backspace
      return 1u << BUTTON_ID_BACK;
    default:
      return 0;
  }
}

static void prv_emit_buttons(uint8_t buttons, PebbleEventType type) {
  for (ButtonId id = 0; id < NUM_BUTTONS; ++id) {
    if (buttons & (1u << id)) {
      PebbleEvent event = {.type = type, .button.button_id = id};
      event_put(&event);
    }
  }
}

static void prv_set_buttons(uint8_t buttons) {
  const uint8_t released = s_report_buttons & ~buttons;
  const uint8_t pressed = buttons & ~s_report_buttons;
  s_report_buttons = buttons;
  prv_emit_buttons(released, PEBBLE_KEYBOARD_BUTTON_UP_EVENT);
  prv_emit_buttons(pressed, PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT);
}

void bt_keyboard_release_buttons(void) {
  s_suppress_enter = false;
  prv_set_buttons(0);
}

void bt_keyboard_suppress_pairing_enter(void) {
  s_suppress_enter = true;
}

void bt_keyboard_handle_report(const uint8_t *report, size_t length) {
  if (!report || length != 8 || report[1] != 0) {
    prv_set_buttons(0);
    return;
  }

  uint8_t buttons = 0;
  bool enter_down = false;
  for (size_t i = 2; i < 8; ++i) {
    // ErrorRollOver, POSTFail and ErrorUndefined provide no usable key state.
    if (report[i] >= 1 && report[i] <= 3) {
      prv_set_buttons(0);
      return;
    }
    if (report[i] == 0x28) {
      enter_down = true;
      if (s_suppress_enter) {
        continue;
      }
    }
    buttons |= prv_button_for_usage(report[i]);
  }
  if (!enter_down) {
    s_suppress_enter = false;
  }
  prv_set_buttons(buttons);
}

bool keyboard_input_filter_event(PebbleEvent *event) {
  const bool keyboard = event->type == PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT ||
                        event->type == PEBBLE_KEYBOARD_BUTTON_UP_EVENT;
  if (!keyboard && event->type != PEBBLE_BUTTON_DOWN_EVENT &&
      event->type != PEBBLE_BUTTON_UP_EVENT) {
    return true;
  }
  if ((unsigned)event->button.button_id >= NUM_BUTTONS) {
    return false;
  }

  const uint8_t button = 1u << event->button.button_id;
  const uint8_t before = s_keyboard_buttons | s_other_buttons;
  uint8_t *source = keyboard ? &s_keyboard_buttons : &s_other_buttons;
  const bool down =
      event->type == PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT || event->type == PEBBLE_BUTTON_DOWN_EVENT;
  if (down) {
    *source |= button;
  } else {
    *source &= ~button;
  }
  if (before == (s_keyboard_buttons | s_other_buttons)) {
    return false;
  }
  event->type = down ? PEBBLE_BUTTON_DOWN_EVENT : PEBBLE_BUTTON_UP_EVENT;
  return true;
}

#endif
