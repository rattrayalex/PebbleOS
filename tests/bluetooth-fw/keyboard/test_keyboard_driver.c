/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdlib.h>
#include <string.h>

#include "clar.h"
#include "keyboard_internal.h"
#include <bluetooth/keyboard.h>
#include <host/ble_hs.h>
#include <host/ble_sm.h>
#include <nimble/nimble_port.h>
#include <os/os_mbuf.h>
#include "stubs_mutex.h"

static struct ble_npl_eventq s_queue;
static struct ble_npl_event *s_events[32];
static size_t s_event_count;
static struct ble_npl_callout *s_callout;
static bool s_timer_active;
static ble_gap_event_fn *s_gap;
static void *s_gap_arg;
static struct ble_gap_conn_desc s_desc;
static struct ble_store_value_sec s_security;
static bool s_bonded;
static int s_commit_error;
static int s_forget_error;
static int s_restore_irk_count;
static int s_connect_count;
static int s_scan_count;
static bool s_advertises_hid = true;
static int s_terminate_count;
static int s_discover_count;
static int s_privacy_count;
static int s_release_count;
static int s_report_count;
static int s_suppress_count;
static int s_passkey_count;
static int s_boot_write_count;
static uint16_t s_descriptor_end;
static ble_gatt_disc_svc_fn *s_service_cb;
static ble_gatt_chr_fn *s_characteristic_cb;
static ble_gatt_dsc_fn *s_descriptor_cb;
static ble_gatt_attr_fn *s_subscribe_cb;
static void *s_gatt_arg;
static ble_addr_t s_saved = {.type = BLE_ADDR_RANDOM, .val = {1, 2, 3, 4, 5, 0xc6}};

void *kernel_malloc_check(size_t size) {
  return malloc(size);
}
void kernel_free(void *ptr) {
  free(ptr);
}
void pbl_sem_give(struct pbl_sem *sem) {
}
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void) {
  return &s_queue;
}
void npl_pebble_eventq_put(struct ble_npl_eventq *queue, struct ble_npl_event *event) {
  cl_assert(s_event_count < 32);
  s_events[s_event_count++] = event;
}
void npl_pebble_callout_init(struct ble_npl_callout *co, struct ble_npl_eventq *queue,
                             ble_npl_event_fn *cb, void *arg) {
  memset(co, 0, sizeof(*co));
  ble_npl_event_init(&co->ev, cb, arg);
  s_callout = co;
}
void npl_pebble_callout_stop(struct ble_npl_callout *co) {
  s_timer_active = false;
}
ble_npl_error_t npl_pebble_callout_reset(struct ble_npl_callout *co, ble_npl_time_t ticks) {
  cl_assert_equal_i(61440, ticks);
  s_timer_active = true;
  return 0;
}
ble_npl_error_t npl_pebble_time_ms_to_ticks(uint32_t ms, ble_npl_time_t *ticks) {
  *ticks = ms * 1024 / 1000;
  return 0;
}

static void prv_drain(void) {
  while (s_event_count) {
    struct ble_npl_event *event = s_events[0];
    memmove(s_events, s_events + 1, --s_event_count * sizeof(*s_events));
    ble_npl_event_run(event);
  }
}

bool nimble_keyboard_store_get(ble_addr_t *peer, char name[32]) {
  if (!s_bonded) {
    return false;
  }
  if (peer) {
    *peer = s_saved;
  }
  if (name) {
    strcpy(name, "Keyboard");
  }
  return true;
}
bool nimble_keyboard_store_matches(const ble_addr_t *peer) {
  return s_bonded && !ble_addr_cmp(peer, &s_saved);
}
void nimble_keyboard_store_load(void) {
}
void nimble_keyboard_store_restore_keys(void) {
}
int nimble_keyboard_store_commit(const char *name) {
  if (!s_commit_error) {
    s_bonded = true;
  }
  return s_commit_error;
}
int nimble_keyboard_store_forget(void) {
  if (!s_forget_error) {
    s_bonded = false;
  }
  return s_forget_error;
}
int nimble_keyboard_store_restore_irk(void) {
  ++s_restore_irk_count;
  return 0;
}
void nimble_keyboard_store_clear_pending(void) {
}
bool nimble_keyboard_store_get_connection_addr(ble_addr_t *peer) {
  *peer = s_saved;
  peer->type += BLE_ADDR_PUBLIC_ID;
  ++s_privacy_count;
  return true;
}
int ble_store_read_peer_sec(const struct ble_store_key_sec *key,
                            struct ble_store_value_sec *value) {
  *value = s_security;
  return 0;
}
int ble_store_util_delete_peer(const ble_addr_t *peer) {
  return 0;
}
int ble_gap_unpair(const ble_addr_t *peer) {
  return 0;
}

void bt_keyboard_release_buttons(void) {
  ++s_release_count;
}
void bt_keyboard_handle_report(const uint8_t *report, size_t length) {
  ++s_report_count;
}
void bt_keyboard_suppress_pairing_enter(void) {
  ++s_suppress_count;
}
int ble_hs_id_infer_auto(int privacy, uint8_t *type) {
  *type = BLE_ADDR_PUBLIC;
  return 0;
}
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *desc) {
  *desc = s_desc;
  return 0;
}
int ble_gap_terminate(uint16_t conn, uint8_t reason) {
  ++s_terminate_count;
  return 0;
}
int ble_gap_disc_cancel(void) {
  return 0;
}
int ble_gap_conn_cancel(void) {
  return 0;
}
int ble_gap_security_initiate(uint16_t conn) {
  return 0;
}
int ble_hs_hci_rand(void *dest, int length) {
  cl_assert_equal_i(sizeof(uint32_t), length);
  *(uint32_t *)dest = 123456;
  return 0;
}
int ble_sm_inject_io(uint16_t conn, struct ble_sm_io *io) {
  cl_assert_equal_i(BLE_SM_IOACT_DISP, io->action);
  ++s_passkey_count;
  return 0;
}
int ble_gap_connect(uint8_t own_type, const ble_addr_t *peer, int32_t duration,
                    const struct ble_gap_conn_params *params, ble_gap_event_fn *cb, void *arg) {
  cl_assert_equal_i(10000, duration);
  cl_assert(params->scan_window < params->scan_itvl);
  ++s_connect_count;
  s_gap = cb;
  s_gap_arg = arg;
  return 0;
}
int ble_gap_disc(uint8_t own_type, int32_t duration, const struct ble_gap_disc_params *params,
                 ble_gap_event_fn *cb, void *arg) {
  ++s_scan_count;
  s_gap = cb;
  s_gap_arg = arg;
  return 0;
}
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *fields, const uint8_t *data, uint8_t length) {
  *fields =
      (struct ble_hs_adv_fields){.appearance_is_present = s_advertises_hid, .appearance = 0x03c1};
  return 0;
}
uint16_t ble_uuid_u16(const ble_uuid_t *uuid) {
  return uuid->type == BLE_UUID_TYPE_16 ? ((const ble_uuid16_t *)uuid)->value : 0;
}
int ble_gattc_disc_svc_by_uuid(uint16_t conn, const ble_uuid_t *uuid, ble_gatt_disc_svc_fn *cb,
                               void *arg) {
  ++s_discover_count;
  s_service_cb = cb;
  s_gatt_arg = arg;
  return 0;
}
int ble_gattc_disc_all_chrs(uint16_t conn, uint16_t start, uint16_t end, ble_gatt_chr_fn *cb,
                            void *arg) {
  s_characteristic_cb = cb;
  return 0;
}
int ble_gattc_disc_all_dscs(uint16_t conn, uint16_t start, uint16_t end, ble_gatt_dsc_fn *cb,
                            void *arg) {
  s_descriptor_end = end;
  s_descriptor_cb = cb;
  return 0;
}
int ble_gattc_write_no_rsp_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t len) {
  cl_assert_equal_i(0, *(const uint8_t *)data);
  ++s_boot_write_count;
  return 0;
}
int ble_gattc_write_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t length,
                         ble_gatt_attr_fn *cb, void *arg) {
  cl_assert_equal_i(1, s_boot_write_count);
  cl_assert_equal_i(1, *(const uint8_t *)data);
  s_subscribe_cb = cb;
  return 0;
}
int os_mbuf_copydata(const struct os_mbuf *om, int offset, int length, void *dest) {
  memcpy(dest, om->om_data + offset, length);
  return 0;
}

static BTKeyboardStatus prv_status(void) {
  BTKeyboardStatus status;
  bt_keyboard_get_status(&status);
  return status;
}
static void prv_gap(struct ble_gap_event event) {
  s_gap(&event, s_gap_arg);
}
static void prv_connected(void) {
  prv_gap((struct ble_gap_event){.type = BLE_GAP_EVENT_CONNECT, .connect = {.conn_handle = 7}});
}
static void prv_encrypted(void) {
  prv_gap(
      (struct ble_gap_event){.type = BLE_GAP_EVENT_ENC_CHANGE, .enc_change = {.conn_handle = 7}});
}
static void prv_start_pair(void) {
  nimble_keyboard_start();
  prv_drain();
  bt_keyboard_pair();
  prv_drain();
  cl_assert_equal_i(BTKeyboardStateScanning, prv_status().state);
  prv_gap((struct ble_gap_event){
    .type = BLE_GAP_EVENT_DISC,
    .disc = {.event_type = BLE_HCI_ADV_RPT_EVTYPE_ADV_IND, .addr = s_saved}
  });
  prv_connected();
}
static void prv_discover(void) {
  struct ble_gatt_error ok = {0}, done = {.status = BLE_HS_EDONE};
  struct ble_gatt_svc service = {.start_handle = 10, .end_handle = 40};
  s_service_cb(7, &ok, &service, s_gatt_arg);
  s_service_cb(7, &done, NULL, s_gatt_arg);
  struct ble_gatt_chr input = {
    .def_handle = 11,
    .val_handle = 12,
    .properties = BLE_GATT_CHR_PROP_NOTIFY,
    .uuid.u16 = BLE_UUID16_INIT(0x2a22)
  };
  struct ble_gatt_chr protocol = {
    .def_handle = 15,
    .val_handle = 16,
    .properties = BLE_GATT_CHR_PROP_WRITE_NO_RSP,
    .uuid.u16 = BLE_UUID16_INIT(0x2a4e)
  };
  s_characteristic_cb(7, &ok, &input, s_gatt_arg);
  s_characteristic_cb(7, &ok, &protocol, s_gatt_arg);
  s_characteristic_cb(7, &done, NULL, s_gatt_arg);
  cl_assert_equal_i(14, s_descriptor_end);
  struct ble_gatt_dsc cccd = {.handle = 13, .uuid.u16 = BLE_UUID16_INIT(0x2902)};
  s_descriptor_cb(7, &ok, 12, &cccd, s_gatt_arg);
  s_descriptor_cb(7, &done, 12, NULL, s_gatt_arg);
}

void test_keyboard_driver__initialize(void) {
  nimble_keyboard_disallow();
  nimble_keyboard_stopped(NULL, NULL);
  prv_drain();
  nimble_keyboard_init();
  s_bonded = false;
  s_scan_count = s_forget_error = s_restore_irk_count = 0;
  s_advertises_hid = true;
  s_commit_error = s_connect_count = s_terminate_count = s_discover_count = s_privacy_count = 0;
  s_release_count = s_report_count = s_suppress_count = s_passkey_count = s_boot_write_count = 0;
  s_service_cb = NULL;
  s_characteristic_cb = NULL;
  s_descriptor_cb = NULL;
  s_subscribe_cb = NULL;
  s_timer_active = false;
  s_desc = (struct ble_gap_conn_desc){
    .conn_handle = 7,
    .peer_id_addr = s_saved,
    .peer_ota_addr = s_saved,
    .sec_state = {.encrypted = 1, .authenticated = 1, .bonded = 1, .key_size = 16}
  };
  s_security = (struct ble_store_value_sec){
    .sc = 1,
    .authenticated = 1,
    .ltk_present = 1,
    .key_size = 16,
    .peer_addr = s_saved
  };
}

void test_keyboard_driver__disabled_commands_do_not_start_radio(void) {
  bt_keyboard_pair();
  bt_keyboard_connect();
  prv_drain();
  cl_assert_equal_i(BTKeyboardStateDisabled, prv_status().state);
  cl_assert_equal_i(0, s_connect_count);
}

void test_keyboard_driver__pairing_requires_authentication_before_discovery(void) {
  prv_start_pair();
  cl_assert_equal_i(0, s_discover_count);
  s_desc.sec_state.authenticated = 0;
  prv_encrypted();
  cl_assert_equal_i(BTKeyboardStateError, prv_status().state);
  cl_assert_equal_i(BLE_HS_EAUTHEN, prv_status().error);
  cl_assert_equal_i(0, s_discover_count);
  cl_assert_equal_i(1, s_terminate_count);
}

void test_keyboard_driver__stored_legacy_key_is_rejected(void) {
  prv_start_pair();
  s_security.sc = 0;
  prv_encrypted();
  cl_assert_equal_i(BLE_HS_EAUTHEN, prv_status().error);
  cl_assert_equal_i(0, s_discover_count);
}

void test_keyboard_driver__passkey_is_displayed_only_for_explicit_pairing(void) {
  prv_start_pair();
  prv_gap((struct ble_gap_event){
    .type = BLE_GAP_EVENT_PASSKEY_ACTION,
    .passkey = {.conn_handle = 7, .params.action = BLE_SM_IOACT_DISP}
  });
  cl_assert(prv_status().passkey_valid);
  cl_assert_equal_i(123456, prv_status().passkey);
  cl_assert_equal_i(1, s_passkey_count);
}

void test_keyboard_driver__reconnect_does_not_silently_pair(void) {
  s_bonded = true;
  nimble_keyboard_start();
  prv_drain();
  prv_connected();
  prv_gap((struct ble_gap_event){
    .type = BLE_GAP_EVENT_PASSKEY_ACTION,
    .passkey = {.conn_handle = 7, .params.action = BLE_SM_IOACT_DISP}
  });
  cl_assert_equal_i(BLE_HS_EAUTHEN, prv_status().error);
  cl_assert_equal_i(0, s_passkey_count);
  cl_assert_equal_i(1, s_terminate_count);
}

void test_keyboard_driver__boot_mode_precedes_subscription_and_pairing_enter_is_suppressed(void) {
  prv_start_pair();
  prv_encrypted();
  prv_discover();
  cl_assert_equal_i(BTKeyboardStateDiscovering, prv_status().state);
  cl_assert_equal_i(0, s_suppress_count);
  struct ble_gatt_error ok = {0};
  s_subscribe_cb(7, &ok, NULL, s_gatt_arg);
  cl_assert_equal_i(BTKeyboardStateConnected, prv_status().state);
  cl_assert(prv_status().bonded);
  cl_assert_equal_i(1, s_suppress_count);
}

void test_keyboard_driver__bond_write_failure_never_enables_input(void) {
  prv_start_pair();
  prv_encrypted();
  prv_discover();
  s_commit_error = BLE_HS_ESTORE_FAIL;
  struct ble_gatt_error ok = {0};
  s_subscribe_cb(7, &ok, NULL, s_gatt_arg);
  cl_assert_equal_i(BTKeyboardStateError, prv_status().state);
  cl_assert(!prv_status().bonded);
  cl_assert_equal_i(1, s_terminate_count);
}

void test_keyboard_driver__cancel_ignores_queued_discovery_completion(void) {
  prv_start_pair();
  prv_encrypted();
  bt_keyboard_disconnect();
  prv_drain();
  struct ble_gatt_error done = {.status = BLE_HS_EDONE};
  s_service_cb(7, &done, NULL, s_gatt_arg);
  cl_assert_equal_i(BTKeyboardStateIdle, prv_status().state);
  cl_assert(s_characteristic_cb == NULL);
}

void test_keyboard_driver__cancel_terminates_late_successful_connect(void) {
  s_bonded = true;
  nimble_keyboard_start();
  prv_drain();
  bt_keyboard_disconnect();
  prv_drain();
  prv_connected();
  cl_assert_equal_i(1, s_terminate_count);
  cl_assert_equal_i(BTKeyboardStateIdle, prv_status().state);
}

void test_keyboard_driver__timeout_retries_but_explicit_disconnect_stops_retry(void) {
  s_bonded = true;
  nimble_keyboard_start();
  prv_drain();
  cl_assert_equal_i(1, s_privacy_count);
  prv_gap((struct ble_gap_event){.type = BLE_GAP_EVENT_CONNECT, .connect.status = BLE_HS_ETIMEOUT});
  cl_assert(s_timer_active);
  ble_npl_event_run(&s_callout->ev);
  prv_drain();
  cl_assert_equal_i(2, s_connect_count);
  cl_assert_equal_i(2, s_privacy_count);
  bt_keyboard_disconnect();
  prv_drain();
  cl_assert(!s_timer_active);
  ble_npl_event_run(&s_callout->ev);
  prv_drain();
  cl_assert_equal_i(2, s_connect_count);
}

void test_keyboard_driver__host_stop_defers_release_and_restores_reconnect_on_start(void) {
  s_bonded = true;
  nimble_keyboard_start();
  prv_drain();
  int releases = s_release_count;
  nimble_keyboard_disallow();
  nimble_keyboard_stopped(NULL, NULL);
  cl_assert_equal_i(releases, s_release_count);
  prv_drain();
  cl_assert(s_release_count > releases);
  cl_assert_equal_i(BTKeyboardStateDisabled, prv_status().state);
  nimble_keyboard_start();
  prv_drain();
  cl_assert_equal_i(2, s_connect_count);
  cl_assert_equal_i(2, s_privacy_count);
}

void test_keyboard_driver__reset_invalidates_already_queued_pair_request(void) {
  nimble_keyboard_start();
  prv_drain();
  bt_keyboard_pair();
  nimble_keyboard_stopped(NULL, NULL);
  nimble_keyboard_resynced();
  prv_drain();
  cl_assert_equal_i(0, s_scan_count);
  cl_assert_equal_i(BTKeyboardStateIdle, prv_status().state);
}

void test_keyboard_driver__forget_works_with_bluetooth_off(void) {
  s_bonded = true;
  bt_keyboard_forget();
  prv_drain();
  cl_assert(!s_bonded);
  cl_assert_equal_i(BTKeyboardStateDisabled, prv_status().state);
}

void test_keyboard_driver__scan_response_requires_prior_connectable_advertisement(void) {
  nimble_keyboard_start();
  prv_drain();
  bt_keyboard_pair();
  prv_drain();
  struct ble_gap_event report = {
    .type = BLE_GAP_EVENT_DISC,
    .disc = {.event_type = BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP, .addr = s_saved}
  };
  prv_gap(report);
  cl_assert_equal_i(0, s_connect_count);
  report.disc.event_type = BLE_HCI_ADV_RPT_EVTYPE_ADV_IND;
  s_advertises_hid = false;
  prv_gap(report);
  cl_assert_equal_i(0, s_connect_count);
  report.disc.event_type = BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
  s_advertises_hid = true;
  prv_gap(report);
  cl_assert_equal_i(1, s_connect_count);
}

void test_keyboard_driver__failed_forget_restores_only_removed_controller_irk(void) {
  s_bonded = true;
  nimble_keyboard_start();
  prv_drain();
  bt_keyboard_disconnect();
  prv_drain();
  cl_assert_equal_i(0, s_restore_irk_count);
  s_forget_error = BLE_HS_ESTORE_FAIL;
  bt_keyboard_forget();
  prv_drain();
  cl_assert(s_bonded);
  cl_assert_equal_i(1, s_restore_irk_count);
  cl_assert_equal_i(BLE_HS_ESTORE_FAIL, prv_status().error);
}

void test_keyboard_driver__notifications_require_ready_connection_and_correct_attribute(void) {
  prv_start_pair();
  struct {
    struct os_mbuf om;
    struct os_mbuf_pkthdr header;
    uint8_t data[8];
  } packet = {0};
  packet.om.om_data = packet.data;
  packet.header.omp_len = 8;
  struct ble_gap_event notification = {
    .type = BLE_GAP_EVENT_NOTIFY_RX,
    .notify_rx = {.conn_handle = 7, .attr_handle = 12, .om = &packet.om}
  };
  prv_gap(notification);
  cl_assert_equal_i(0, s_report_count);
  prv_encrypted();
  prv_discover();
  prv_gap(notification);
  cl_assert_equal_i(0, s_report_count);
  struct ble_gatt_error ok = {0};
  s_subscribe_cb(7, &ok, NULL, s_gatt_arg);
  notification.notify_rx.attr_handle = 17;
  prv_gap(notification);
  cl_assert_equal_i(0, s_report_count);
  notification.notify_rx.attr_handle = 12;
  prv_gap(notification);
  cl_assert_equal_i(1, s_report_count);
  int releases = s_release_count;
  packet.header.omp_len = 7;
  prv_gap(notification);
  cl_assert_equal_i(1, s_report_count);
  cl_assert_equal_i(releases + 1, s_release_count);
  bt_keyboard_disconnect();
  prv_drain();
  packet.header.omp_len = 8;
  prv_gap(notification);
  cl_assert_equal_i(1, s_report_count);
}

static void prv_stop_complete(void *context) {
  bool *completed = context;
  cl_assert(s_release_count > 0);
  *completed = true;
}

void test_keyboard_driver__stop_completion_waits_for_host_cleanup_even_if_reset_supersedes(void) {
  bool completed = false;
  nimble_keyboard_stopped(prv_stop_complete, &completed);
  nimble_keyboard_stopped(NULL, NULL);
  cl_assert(!completed);
  prv_drain();
  cl_assert(completed);
}
