/* SPDX-FileCopyrightText: 2025 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#ifndef _NIMBLE_STORE_H_
#define _NIMBLE_STORE_H_

void nimble_store_init(void);
void nimble_store_unload(void);

#ifdef CONFIG_BT_KEYBOARD
#include <host/ble_store.h>
void nimble_store_restore_keyboard_sec(int obj_type, const struct ble_store_value_sec *value);
#endif

#endif
