/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/events.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Called serially on the Bluetooth host task.
void bt_keyboard_handle_report(const uint8_t *report, size_t length);
void bt_keyboard_release_buttons(void);
void bt_keyboard_suppress_pairing_enter(void);

// Called on KernelMain before button events reach the shell or subscribers.
// Returns false for redundant transitions; translates keyboard events in place.
bool keyboard_input_filter_event(PebbleEvent *event);
