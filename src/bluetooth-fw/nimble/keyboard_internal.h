/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <host/ble_store.h>

void nimble_keyboard_init(void);
void nimble_keyboard_prepare(void);
void nimble_keyboard_start(void);
void nimble_keyboard_disallow(void);
//! Complete cleanup on the host thread before invoking the optional callback.
void nimble_keyboard_stopped(void (*complete)(void *), void *context);
void nimble_keyboard_resynced(void);
bool nimble_keyboard_owns_peer(const ble_addr_t *peer);

//! All store operations run on the host thread, including the load before host startup.
void nimble_keyboard_store_load(void);
void nimble_keyboard_store_restore_keys(void);
bool nimble_keyboard_store_get_connection_addr(ble_addr_t *peer);
//! Only after a successful unpair whose subsequent persistent deletion failed.
int nimble_keyboard_store_restore_irk(void);
bool nimble_keyboard_store_get(ble_addr_t *peer, char name[32]);
bool nimble_keyboard_store_matches(const ble_addr_t *peer);
int nimble_keyboard_store_capture(int obj_type, const struct ble_store_value_sec *value);
int nimble_keyboard_store_commit(const char *name);
int nimble_keyboard_store_forget(void);
void nimble_keyboard_store_clear_pending(void);
