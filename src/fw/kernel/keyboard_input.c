/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include "keyboard_input.h"

#ifdef CONFIG_BT_KEYBOARD

#include "kernel/event_loop.h"

#include <pbl/kernel/mutex.h>

// The Bluetooth host owns the report state; KernelMain owns the two input sources. One
// mutex-protected hand-off carries reports across, and the host task queues at most one delivery
// callback at a time so a chatty peer cannot fill the kernel event queue.
static PBL_MUTEX_DEFINE(s_report_mutex);
static uint8_t s_report_target;   // Buttons the most recent report held down.
static uint8_t s_report_seen;     // Buttons held at any point since the last delivery.
static uint8_t s_report_released; // Buttons released at any point since the last delivery.
static bool s_report_pending;     // A delivery callback is queued.
static uint8_t s_report_buttons;  // Buttons KernelMain has already been told about.
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

// KernelMain. The four emitted sets are disjoint, so a delivery emits at most two events per
// button: eight, inside the fourteen KernelMain may post to itself while handling one event
// (MAX_FROM_KERNEL_MAIN_EVENTS in kernel/events.c). KernelMain drains that queue ahead of every
// other one, so the seven still pending while it handles the first leave room for what a button
// handler posts to itself. Physical buttons do not use this queue; they arrive by interrupt.
void keyboard_input_deliver_report(void *unused) {
  pbl_mutex_lock(&s_report_mutex, PBL_FOREVER);
  // Cleared first: a report that lands during delivery queues the next callback rather than
  // extending this one, so the peer cannot starve KernelMain.
  s_report_pending = false;
  const uint8_t target = s_report_target;
  const uint8_t seen = s_report_seen;
  const uint8_t released = s_report_released;
  s_report_seen = target;
  s_report_released = 0;
  pbl_mutex_unlock(&s_report_mutex);

  // Held at neither end of the window but pressed inside it, and its mirror: held at both ends but
  // released inside it. Neither shows up in a plain before-and-after comparison. What a bounded
  // encoding cannot carry is order or repetition: these are emitted per button rather than in the
  // order the peer sent them, and three or more transitions of one button inside one window
  // collapse to one edge. Per-button click recognition is unaffected.
  const uint8_t tapped = seen & ~target & ~s_report_buttons;
  const uint8_t retaken = released & target & s_report_buttons;
  prv_emit_buttons(s_report_buttons & ~target, PEBBLE_KEYBOARD_BUTTON_UP_EVENT);
  s_report_buttons &= target;
  prv_emit_buttons(target & ~s_report_buttons, PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT);
  s_report_buttons = target;
  prv_emit_buttons(retaken, PEBBLE_KEYBOARD_BUTTON_UP_EVENT);
  prv_emit_buttons(retaken, PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT);
  prv_emit_buttons(tapped, PEBBLE_KEYBOARD_BUTTON_DOWN_EVENT);
  prv_emit_buttons(tapped, PEBBLE_KEYBOARD_BUTTON_UP_EVENT);
}

// Bluetooth host task.
static void prv_set_buttons(uint8_t buttons) {
  pbl_mutex_lock(&s_report_mutex, PBL_FOREVER);
  const bool changed =
      s_report_target != buttons || s_report_seen != buttons || s_report_released != 0;
  s_report_released |= s_report_target & ~buttons;
  s_report_target = buttons;
  s_report_seen |= buttons;
  const bool schedule = changed && !s_report_pending;
  s_report_pending |= schedule;
  pbl_mutex_unlock(&s_report_mutex);
  if (schedule) {
    launcher_task_add_callback(keyboard_input_deliver_report, NULL);
  }
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
