/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include "keyboard_internal.h"
#include "nimble_store.h"

#include <host/ble_hs.h>
#include <pbl/services/settings/settings_file.h>
#include <string.h>

// A separate, versioned record keeps accessory credentials out of the gateway database.
// SC uses the same LTK in both directions and has no EDIV or RAND.
typedef struct {
  uint8_t version;
  uint8_t addr_type;
  uint8_t addr[6];
  uint8_t ltk[16];
  uint8_t irk[16];
  uint8_t has_irk;
  char name[32];
} KeyboardBond;

_Static_assert(sizeof(KeyboardBond) == 73, "Changing the bond record requires a version migration");

static const char s_file_name[] = "bt_keyboard";
static const uint8_t s_bond_key = 1;
static KeyboardBond s_bond;
static KeyboardBond s_pending;
static bool s_valid;
static bool s_pending_valid;

void nimble_keyboard_store_load(void) {
  SettingsFile file;
  s_valid = false;
  s_pending_valid = false;
  if (settings_file_open(&file, s_file_name, 256) != S_SUCCESS) {
    return;
  }
  if (settings_file_get_len(&file, &s_bond_key, sizeof(s_bond_key)) == sizeof(s_bond) &&
      settings_file_get(&file, &s_bond_key, sizeof(s_bond_key), &s_bond, sizeof(s_bond)) ==
          S_SUCCESS &&
      s_bond.version == 1 && s_bond.addr_type <= BLE_ADDR_RANDOM && s_bond.has_irk <= 1 &&
      s_bond.name[sizeof(s_bond.name) - 1] == '\0') {
    s_valid = true;
  }
  settings_file_close(&file);
  if (!s_valid) {
    memset(&s_bond, 0, sizeof(s_bond));
    return;
  }

  nimble_keyboard_store_restore_keys();
}

bool nimble_keyboard_store_get(ble_addr_t *peer, char name[32]) {
  if (!s_valid) {
    return false;
  }
  if (peer) {
    peer->type = s_bond.addr_type;
    memcpy(peer->val, s_bond.addr, sizeof(peer->val));
  }
  if (name) {
    memcpy(name, s_bond.name, sizeof(s_bond.name));
  }
  return true;
}

bool nimble_keyboard_store_matches(const ble_addr_t *peer) {
  ble_addr_t stored;
  return nimble_keyboard_store_get(&stored, NULL) && ble_addr_cmp(peer, &stored) == 0;
}

int nimble_keyboard_store_capture(int obj_type, const struct ble_store_value_sec *value) {
  if (!value->sc || !value->authenticated || value->key_size != 16 || value->csrk_present ||
      value->peer_addr.type > BLE_ADDR_RANDOM) {
    return BLE_HS_EAUTHEN;
  }
  if (obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC && value->ltk_present) {
    s_pending = (KeyboardBond){
      .version = 1,
      .addr_type = value->peer_addr.type,
      .has_irk = value->irk_present,
    };
    memcpy(s_pending.addr, value->peer_addr.val, sizeof(s_pending.addr));
    memcpy(s_pending.ltk, value->ltk, sizeof(s_pending.ltk));
    memcpy(s_pending.irk, value->irk, sizeof(s_pending.irk));
    s_pending_valid = true;
  }
  return 0;
}

int nimble_keyboard_store_commit(const char *name) {
  if (s_valid) {
    return 0;
  }
  if (!s_pending_valid) {
    return BLE_HS_EAUTHEN;
  }
  strncpy(s_pending.name, name, sizeof(s_pending.name) - 1);
  s_pending.name[sizeof(s_pending.name) - 1] = '\0';
  SettingsFile file;
  if (settings_file_open(&file, s_file_name, 256) != S_SUCCESS) {
    return BLE_HS_ESTORE_FAIL;
  }
  int rc = settings_file_set(&file, &s_bond_key, sizeof(s_bond_key), &s_pending, sizeof(s_pending));
  settings_file_close(&file);
  if (rc != S_SUCCESS) {
    return BLE_HS_ESTORE_FAIL;
  }
  s_bond = s_pending;
  s_valid = true;
  nimble_keyboard_store_clear_pending();
  return 0;
}

int nimble_keyboard_store_forget(void) {
  SettingsFile file;
  if (settings_file_open(&file, s_file_name, 256) != S_SUCCESS) {
    return BLE_HS_ESTORE_FAIL;
  }
  int rc = S_SUCCESS;
  if (settings_file_exists(&file, &s_bond_key, sizeof(s_bond_key))) {
    rc = settings_file_delete(&file, &s_bond_key, sizeof(s_bond_key));
  }
  settings_file_close(&file);
  if (rc != S_SUCCESS) {
    return BLE_HS_ESTORE_FAIL;
  }
  s_valid = false;
  memset(&s_bond, 0, sizeof(s_bond));
  nimble_keyboard_store_clear_pending();
  return 0;
}

void nimble_keyboard_store_clear_pending(void) {
  s_pending_valid = false;
  memset(&s_pending, 0, sizeof(s_pending));
}

bool nimble_keyboard_store_get_connection_addr(ble_addr_t *peer) {
  if (!nimble_keyboard_store_get(peer, NULL)) {
    return false;
  }
  // Host synchronization and initial pairing already install the peer IRK.
  if (s_bond.has_irk) {
    peer->type += BLE_ADDR_PUBLIC_ID;
  }
  return true;
}

int nimble_keyboard_store_restore_irk(void) {
  if (!s_valid || !s_bond.has_irk) {
    return 0;
  }
  ble_addr_t peer;
  nimble_keyboard_store_get(&peer, NULL);
  struct ble_store_key_sec key = {.peer_addr = peer};
  struct ble_store_value_sec value;
  int rc = ble_store_read_peer_sec(&key, &value);
  if (!rc) {
    rc = ble_store_write_peer_sec(&value);
  }
  return rc;
}

void nimble_keyboard_store_restore_keys(void) {
  if (!s_valid) {
    return;
  }
  struct ble_store_value_sec value = {
    .peer_addr.type = s_bond.addr_type,
    .key_size = 16,
    .ltk_present = 1,
    .sc = 1,
    .authenticated = 1,
  };
  memcpy(value.peer_addr.val, s_bond.addr, sizeof(s_bond.addr));
  memcpy(value.ltk, s_bond.ltk, sizeof(value.ltk));
  nimble_store_restore_keyboard_sec(BLE_STORE_OBJ_TYPE_OUR_SEC, &value);
  value.irk_present = s_bond.has_irk;
  memcpy(value.irk, s_bond.irk, sizeof(value.irk));
  nimble_store_restore_keyboard_sec(BLE_STORE_OBJ_TYPE_PEER_SEC, &value);
}
