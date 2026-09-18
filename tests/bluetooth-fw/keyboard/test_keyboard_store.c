/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include "keyboard_internal.h"
#include "nimble_store.h"
#include <host/ble_hs.h>
#include <pbl/services/settings/settings_file.h>

#include "clar.h"

#include <string.h>

static uint8_t s_record[128];
static int s_record_length;
static bool s_io_failure;
static bool s_write_failure;
static bool s_delete_failure;
static struct ble_store_value_sec s_restored[2];
static size_t s_restore_count;

status_t settings_file_open(SettingsFile *file, const char *name, int max_used_space) {
  cl_assert_equal_s("bt_keyboard", name);
  return s_io_failure ? E_INTERNAL : S_SUCCESS;
}

void settings_file_close(SettingsFile *file) {
}

bool settings_file_exists(SettingsFile *file, const void *key, size_t key_len) {
  return s_record_length != 0;
}

int settings_file_get_len(SettingsFile *file, const void *key, size_t key_len) {
  return s_record_length;
}

status_t settings_file_get(SettingsFile *file, const void *key, size_t key_len, void *out,
                           size_t length) {
  cl_assert_equal_i(s_record_length, length);
  memcpy(out, s_record, length);
  return S_SUCCESS;
}

status_t settings_file_set(SettingsFile *file, const void *key, size_t key_len, const void *value,
                           size_t length) {
  cl_assert(length <= sizeof(s_record));
  if (s_write_failure) {
    return E_INTERNAL;
  }
  memcpy(s_record, value, length);
  s_record_length = length;
  return S_SUCCESS;
}

status_t settings_file_delete(SettingsFile *file, const void *key, size_t key_len) {
  if (s_delete_failure) {
    return E_INTERNAL;
  }
  s_record_length = 0;
  return S_SUCCESS;
}

void nimble_store_restore_keyboard_sec(int obj_type, const struct ble_store_value_sec *value) {
  cl_assert(s_restore_count < 2);
  cl_assert_equal_i(s_restore_count == 0 ? BLE_STORE_OBJ_TYPE_OUR_SEC : BLE_STORE_OBJ_TYPE_PEER_SEC,
                    obj_type);
  s_restored[s_restore_count++] = *value;
}

static struct ble_store_value_sec prv_security(void) {
  struct ble_store_value_sec value = {
    .peer_addr = {.type = BLE_ADDR_RANDOM, .val = {1, 2, 3, 4, 5, 0xc6}},
    .key_size = 16,
    .sc = 1,
    .authenticated = 1,
    .ltk_present = 1,
    .irk_present = 1,
  };
  memset(value.ltk, 0x42, sizeof(value.ltk));
  memset(value.irk, 0x19, sizeof(value.irk));
  return value;
}

static void prv_save_bond(void) {
  struct ble_store_value_sec value = prv_security();
  cl_assert_equal_i(0, nimble_keyboard_store_capture(BLE_STORE_OBJ_TYPE_PEER_SEC, &value));
  cl_assert_equal_i(0, nimble_keyboard_store_commit("Test keyboard"));
}

void test_keyboard_store__initialize(void) {
  memset(s_record, 0, sizeof(s_record));
  memset(s_restored, 0, sizeof(s_restored));
  s_record_length = 0;
  s_restore_count = 0;
  s_io_failure = false;
  s_write_failure = false;
  s_delete_failure = false;
  nimble_keyboard_store_load();
}

void test_keyboard_store__empty_store_is_unbonded(void) {
  cl_assert(!nimble_keyboard_store_get(NULL, NULL));
  cl_assert_equal_i(0, s_restore_count);
  cl_assert_equal_i(BLE_HS_EAUTHEN, nimble_keyboard_store_commit("Missing keys"));
}

void test_keyboard_store__roundtrip_restores_authenticated_keys_and_identity(void) {
  prv_save_bond();
  nimble_keyboard_store_load();
  cl_assert_equal_i(2, s_restore_count);
  struct ble_store_value_sec expected = prv_security();
  for (size_t i = 0; i < s_restore_count; ++i) {
    cl_assert_equal_i(16, s_restored[i].key_size);
    cl_assert(s_restored[i].sc && s_restored[i].authenticated && s_restored[i].ltk_present);
    cl_assert_equal_m(expected.ltk, s_restored[i].ltk, sizeof(expected.ltk));
    cl_assert_equal_i(0, ble_addr_cmp(&expected.peer_addr, &s_restored[i].peer_addr));
  }
  cl_assert(s_restored[1].irk_present);
  cl_assert_equal_m(expected.irk, s_restored[1].irk, sizeof(expected.irk));
  ble_addr_t peer;
  char name[32];
  cl_assert(nimble_keyboard_store_get(&peer, name));
  cl_assert_equal_s("Test keyboard", name);
  cl_assert(nimble_keyboard_store_matches(&peer));
  peer.val[0] ^= 1;
  cl_assert(!nimble_keyboard_store_matches(&peer));
}

void test_keyboard_store__rejects_insecure_keys(void) {
  for (int invalid = 0; invalid < 5; ++invalid) {
    struct ble_store_value_sec value = prv_security();
    switch (invalid) {
      case 0:
        value.sc = 0;
        break;
      case 1:
        value.authenticated = 0;
        break;
      case 2:
        value.key_size = 7;
        break;
      case 3:
        value.csrk_present = 1;
        break;
      case 4:
        value.peer_addr.type = BLE_ADDR_RANDOM_ID;
        break;
    }
    cl_assert_equal_i(BLE_HS_EAUTHEN,
                      nimble_keyboard_store_capture(BLE_STORE_OBJ_TYPE_PEER_SEC, &value));
    cl_assert_equal_i(BLE_HS_EAUTHEN, nimble_keyboard_store_commit("Invalid"));
    cl_assert(!nimble_keyboard_store_get(NULL, NULL));
  }
}

void test_keyboard_store__requires_peer_ltk(void) {
  struct ble_store_value_sec value = prv_security();
  cl_assert_equal_i(0, nimble_keyboard_store_capture(BLE_STORE_OBJ_TYPE_OUR_SEC, &value));
  cl_assert_equal_i(BLE_HS_EAUTHEN, nimble_keyboard_store_commit("No peer key"));
  value.ltk_present = 0;
  cl_assert_equal_i(0, nimble_keyboard_store_capture(BLE_STORE_OBJ_TYPE_PEER_SEC, &value));
  cl_assert_equal_i(BLE_HS_EAUTHEN, nimble_keyboard_store_commit("No peer key"));
}

void test_keyboard_store__cancel_discards_pending_keys(void) {
  struct ble_store_value_sec value = prv_security();
  cl_assert_equal_i(0, nimble_keyboard_store_capture(BLE_STORE_OBJ_TYPE_PEER_SEC, &value));
  nimble_keyboard_store_clear_pending();
  cl_assert_equal_i(BLE_HS_EAUTHEN, nimble_keyboard_store_commit("Cancelled"));
}

void test_keyboard_store__failed_save_does_not_claim_bond(void) {
  struct ble_store_value_sec value = prv_security();
  cl_assert_equal_i(0, nimble_keyboard_store_capture(BLE_STORE_OBJ_TYPE_PEER_SEC, &value));
  s_write_failure = true;
  cl_assert_equal_i(BLE_HS_ESTORE_FAIL, nimble_keyboard_store_commit("Unwritten"));
  cl_assert(!nimble_keyboard_store_get(NULL, NULL));
}

void test_keyboard_store__forget_failure_preserves_bond(void) {
  prv_save_bond();
  s_delete_failure = true;
  cl_assert_equal_i(BLE_HS_ESTORE_FAIL, nimble_keyboard_store_forget());
  cl_assert(nimble_keyboard_store_get(NULL, NULL));
  s_delete_failure = false;
  cl_assert_equal_i(0, nimble_keyboard_store_forget());
  cl_assert(!nimble_keyboard_store_get(NULL, NULL));
  nimble_keyboard_store_load();
  cl_assert_equal_i(0, s_restore_count);
}

void test_keyboard_store__failed_forget_restores_keys_without_reading_failed_flash(void) {
  prv_save_bond();
  s_delete_failure = true;
  cl_assert_equal_i(BLE_HS_ESTORE_FAIL, nimble_keyboard_store_forget());
  s_io_failure = true;
  nimble_keyboard_store_restore_keys();
  cl_assert_equal_i(2, s_restore_count);
  cl_assert(nimble_keyboard_store_get(NULL, NULL));
  struct ble_store_value_sec expected = prv_security();
  cl_assert_equal_m(expected.ltk, s_restored[1].ltk, sizeof(expected.ltk));
  cl_assert_equal_m(expected.irk, s_restored[1].irk, sizeof(expected.irk));
}

void test_keyboard_store__corrupt_or_incomplete_records_are_ignored(void) {
  prv_save_bond();
  s_record[0] = 0xff;
  nimble_keyboard_store_load();
  cl_assert(!nimble_keyboard_store_get(NULL, NULL));
  cl_assert_equal_i(0, s_restore_count);
  prv_save_bond();
  --s_record_length;
  nimble_keyboard_store_load();
  cl_assert(!nimble_keyboard_store_get(NULL, NULL));
  cl_assert_equal_i(0, s_restore_count);
}

void test_keyboard_store__name_is_terminated(void) {
  struct ble_store_value_sec value = prv_security();
  cl_assert_equal_i(0, nimble_keyboard_store_capture(BLE_STORE_OBJ_TYPE_PEER_SEC, &value));
  const char *long_name = "A keyboard name that is longer than thirty one bytes";
  cl_assert_equal_i(0, nimble_keyboard_store_commit(long_name));
  nimble_keyboard_store_load();
  char name[32];
  cl_assert(nimble_keyboard_store_get(NULL, name));
  cl_assert_equal_i(31, strlen(name));
  cl_assert_equal_m(long_name, name, 31);
}

void test_keyboard_store__privacy_reconnect_uses_identity_address(void) {
  prv_save_bond();
  nimble_keyboard_store_load();
  ble_addr_t peer;
  cl_assert(nimble_keyboard_store_get_connection_addr(&peer));
  cl_assert_equal_i(BLE_ADDR_RANDOM_ID, peer.type);
  cl_assert_equal_m(s_restored[1].peer_addr.val, peer.val, sizeof(peer.val));
}

void test_keyboard_store__no_irk_keeps_normal_address_type(void) {
  struct ble_store_value_sec value = prv_security();
  value.irk_present = 0;
  cl_assert_equal_i(0, nimble_keyboard_store_capture(BLE_STORE_OBJ_TYPE_PEER_SEC, &value));
  cl_assert_equal_i(0, nimble_keyboard_store_commit("No privacy"));
  nimble_keyboard_store_load();
  ble_addr_t peer;
  cl_assert(nimble_keyboard_store_get_connection_addr(&peer));
  cl_assert_equal_i(BLE_ADDR_RANDOM, peer.type);
}
