/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  BTKeyboardStateDisabled,
  BTKeyboardStateIdle,
  BTKeyboardStateScanning,
  BTKeyboardStateConnecting,
  BTKeyboardStatePairing,
  BTKeyboardStateDiscovering,
  BTKeyboardStateConnected,
  BTKeyboardStateError,
} BTKeyboardState;

typedef struct {
  BTKeyboardState state;
  bool bonded;
  bool passkey_valid;
  uint32_t passkey;
  char name[32];
  int error;
} BTKeyboardStatus;

//! These APIs copy status or schedule work on the Bluetooth host thread.
void bt_keyboard_get_status(BTKeyboardStatus *out);
//! Explicitly select the first advertising HID device; authenticated pairing is required.
void bt_keyboard_pair(void);
void bt_keyboard_connect(void);
//! Also cancels discovery, connection establishment, and pairing.
void bt_keyboard_disconnect(void);
void bt_keyboard_forget(void);

//! Input callbacks, serialized on the Bluetooth host thread.
void bt_keyboard_handle_report(const uint8_t *report, size_t length);
void bt_keyboard_release_buttons(void);
void bt_keyboard_suppress_pairing_enter(void);
