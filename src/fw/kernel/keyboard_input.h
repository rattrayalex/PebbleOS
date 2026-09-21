/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/events.h"

#include <bluetooth/keyboard.h>

// Called on KernelMain before button events reach the shell or subscribers.
// Returns false for redundant transitions; translates keyboard events in place.
bool keyboard_input_filter_event(PebbleEvent *event);

// KernelMain callback that turns the latest keyboard report into button events. Exposed for tests;
// the Bluetooth host queues it through launcher_task_add_callback.
void keyboard_input_deliver_report(void *unused);
