/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdlib.h>
#include <string.h>

#include "clar.h"
#include "keyboard_internal.h"
#include "kernel/event_loop.h"
#include <bluetooth/keyboard.h>
#include <host/ble_hs.h>
#include <host/ble_sm.h>
#include <nimble/nimble_port.h>
#include <os/os_mbuf.h>
#define CUSTOM_LOG_INTERNAL
#include "stubs_logging.h"
#include "stubs_mutex.h"

static struct ble_npl_eventq s_queue;
static unsigned s_warning_count;
static struct ble_npl_event *s_events[32];
static size_t s_event_count;
static struct ble_npl_callout *s_callout;
static bool s_timer_active;
static ble_gap_event_fn *s_gap;
static void *s_gap_arg;
static struct ble_gap_conn_desc s_desc;
static struct ble_store_value_sec s_security;
static bool s_bonded;
static int s_conn_find_error;
static int s_security_start_error;
static struct ble_gap_conn_params s_connect_params;
static CallbackEventCallback s_kernel_callback;
static void *s_kernel_context;
static int s_status_events;
static bool s_in_kernel_callback;
static ble_addr_t s_bond_peers[3];
static size_t s_bond_peer_count;
static void prv_drain_kernel(void);
static int s_commit_error;
static int s_forget_error;
static int s_restore_irk_count;
static bool s_host_enabled;
static int s_unpair_count;
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

static void log_internal(uint8_t log_level, const char *src_filename, int src_line_number,
                         const char *fmt, va_list args) {
  if (log_level == LOG_LEVEL_WARNING || log_level == LOG_LEVEL_ERROR) {
    ++s_warning_count;
  }
}

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
  ++s_unpair_count;
  return 0;
}
int ble_hs_is_enabled(void) {
  return s_host_enabled;
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
  if (s_conn_find_error || conn != s_desc.conn_handle) {
    return s_conn_find_error ? s_conn_find_error : BLE_HS_ENOTCONN;
  }
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
  return s_security_start_error;
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
  s_connect_params = *params;
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
  prv_drain_kernel();
  nimble_keyboard_init();
  s_bonded = false;
  s_conn_find_error = s_security_start_error = 0;
  s_bond_peer_count = 0;
  s_kernel_callback = NULL;
  s_kernel_context = NULL;
  s_status_events = 0;
  s_warning_count = 0;
  s_in_kernel_callback = false;
  s_host_enabled = true;
  s_unpair_count = 0;
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
  s_host_enabled = false;
  s_bonded = true;
  bt_keyboard_forget();
  prv_drain();
  cl_assert(!s_bonded);
  cl_assert_equal_i(0, s_unpair_count);
  cl_assert_equal_i(BTKeyboardStateDisabled, prv_status().state);
}

void test_keyboard_driver__forget_removes_controller_irk_before_commands_are_enabled(void) {
  s_bonded = true;
  bt_keyboard_forget();
  prv_drain();
  cl_assert(!s_bonded);
  cl_assert_equal_i(1, s_unpair_count);
  cl_assert_equal_i(BTKeyboardStateDisabled, prv_status().state);
}

void test_keyboard_driver__failed_forget_restores_irk_before_commands_are_enabled(void) {
  s_bonded = true;
  s_forget_error = BLE_HS_ESTORE_FAIL;
  bt_keyboard_forget();
  prv_drain();
  cl_assert(s_bonded);
  cl_assert_equal_i(1, s_unpair_count);
  cl_assert_equal_i(1, s_restore_irk_count);
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

void launcher_task_add_callback(CallbackEventCallback callback, void *context) {
  cl_assert(s_kernel_callback == NULL);
  s_kernel_callback = callback;
  s_kernel_context = context;
}

void event_put(PebbleEvent *event) {
  cl_assert(s_in_kernel_callback);
  cl_assert_equal_i(PEBBLE_BT_KEYBOARD_STATUS_CHANGED_EVENT, event->type);
  ++s_status_events;
  // The event consumer can read status, with the driver's status mutex released.
  bt_keyboard_get_status(&(BTKeyboardStatus){0});
}

static void prv_drain_kernel(void) {
  if (s_kernel_callback) {
    CallbackEventCallback callback = s_kernel_callback;
    s_kernel_callback = NULL;
    s_in_kernel_callback = true;
    callback(s_kernel_context);
    s_in_kernel_callback = false;
  }
}

void test_keyboard_driver__reconnect_reports_encryption_without_pairing(void) {
  s_bonded = true;
  nimble_keyboard_start();
  prv_drain();
  prv_connected();
  cl_assert_equal_i(BTKeyboardStateEncrypting, prv_status().state);
  cl_assert(!prv_status().passkey_valid);
}

void test_keyboard_driver__transient_encryption_failure_keeps_background_reconnect(void) {
  s_bonded = true;
  nimble_keyboard_start();
  prv_drain();
  prv_connected();
  // NimBLE sends pairing completion before the more precise encryption status.
  prv_gap((struct ble_gap_event){
    .type = BLE_GAP_EVENT_PAIRING_COMPLETE,
    .pairing_complete = {.conn_handle = 7, .status = BLE_SM_ERR_UNSPECIFIED}
  });
  prv_gap((struct ble_gap_event){
    .type = BLE_GAP_EVENT_ENC_CHANGE,
    .enc_change = {.conn_handle = 7, .status = BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO)}
  });
  cl_assert(s_timer_active);
  cl_assert_equal_i(0, s_discover_count);
  prv_gap((struct ble_gap_event){.type = BLE_GAP_EVENT_DISCONNECT, .disconnect.conn = s_desc});
  ble_npl_event_run(&s_callout->ev);
  prv_drain();
  cl_assert_equal_i(2, s_connect_count);
}

void test_keyboard_driver__explicit_connect_scans_faster_than_background_retry(void) {
  s_bonded = true;
  nimble_keyboard_start();
  prv_drain();
  struct ble_gap_conn_params background = s_connect_params;
  bt_keyboard_disconnect();
  prv_drain();
  bt_keyboard_connect();
  prv_drain();
  cl_assert(s_connect_params.scan_window * background.scan_itvl >
            background.scan_window * s_connect_params.scan_itvl);
}

void test_keyboard_driver__connection_updates_bound_interval_and_keep_sleep_latency(void) {
  prv_start_pair();
  struct ble_gap_upd_params request =
      {.itvl_min = 6, .itvl_max = 3200, .latency = 499, .supervision_timeout = 10};
  struct ble_gap_upd_params result = {0};
  prv_gap((struct ble_gap_event){
    .type = BLE_GAP_EVENT_CONN_UPDATE_REQ,
    .conn_update_req = {.conn_handle = 7, .peer_params = &request, .self_params = &result}
  });
  cl_assert(result.itvl_max <= 160);
  cl_assert(result.itvl_min >= 6);
  cl_assert(result.itvl_min <= result.itvl_max);
  cl_assert(result.latency > 0);
  cl_assert((result.latency + 1) * result.itvl_max <= 3200);
  cl_assert(result.supervision_timeout * 4 > (result.latency + 1) * result.itvl_max);
}

void test_keyboard_driver__status_events_coalesce_and_run_on_kernel_main(void) {
  nimble_keyboard_start();
  prv_drain();
  cl_assert_equal_i(0, s_status_events);
  cl_assert(s_kernel_callback != NULL);
  bt_keyboard_pair();
  prv_drain();
  prv_drain_kernel();
  cl_assert_equal_i(1, s_status_events);
  cl_assert_equal_i(BTKeyboardStateScanning, prv_status().state);
}

int ble_store_iterate(int obj_type, ble_store_iterator_fn *callback, void *context) {
  cl_assert_equal_i(BLE_STORE_OBJ_TYPE_PEER_SEC, obj_type);
  for (size_t i = 0; i < s_bond_peer_count; ++i) {
    union ble_store_value value = {.sec.peer_addr = s_bond_peers[i]};
    int rc = callback(obj_type, &value, context);
    if (rc) {
      return rc;
    }
  }
  return 0;
}

void test_keyboard_driver__authentication_failure_does_not_retry(void) {
  const int failures[] = {
    BLE_HS_EAUTHEN, BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL), BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)
  };
  for (size_t i = 0; i < sizeof(failures) / sizeof(*failures); ++i) {
    test_keyboard_driver__initialize();
    s_bonded = true;
    nimble_keyboard_start();
    prv_drain();
    prv_connected();
    prv_gap((struct ble_gap_event){
      .type = BLE_GAP_EVENT_ENC_CHANGE,
      .enc_change = {.conn_handle = 7, .status = failures[i]}
    });
    cl_assert_equal_i(BTKeyboardStateError, prv_status().state);
    cl_assert(!s_timer_active);
    cl_assert_equal_i(0, s_discover_count);
  }
}

void test_keyboard_driver__busy_security_start_retries_saved_bond_only(void) {
  s_bonded = true;
  nimble_keyboard_start();
  prv_drain();
  s_security_start_error = BLE_HS_EBUSY;
  prv_connected();
  cl_assert(s_timer_active);
  test_keyboard_driver__initialize();
  s_security_start_error = BLE_HS_EBUSY;
  prv_start_pair();
  cl_assert(!s_timer_active);
}

void test_keyboard_driver__ownership_requires_exact_saved_identity_type(void) {
  s_bonded = true;
  cl_assert(nimble_keyboard_owns_peer(&s_saved));
  ble_addr_t phone = s_saved;
  phone.type = BLE_ADDR_PUBLIC;
  cl_assert(!nimble_keyboard_owns_peer(&phone));
  phone = s_saved;
  ++phone.val[0];
  cl_assert(!nimble_keyboard_owns_peer(&phone));
}

void test_keyboard_driver__live_identity_lookup_failure_never_owns_an_unrelated_phone(void) {
  prv_start_pair();
  ble_addr_t identity = {.type = BLE_ADDR_PUBLIC, .val = {9, 8, 7, 6, 5, 4}};
  s_desc.peer_id_addr = identity;
  cl_assert(nimble_keyboard_owns_peer(&identity));
  cl_assert(nimble_keyboard_owns_peer(&s_desc.peer_ota_addr));
  s_conn_find_error = BLE_HS_ENOTCONN;
  cl_assert(!nimble_keyboard_owns_peer(&identity));
  cl_assert(nimble_keyboard_owns_peer(&s_saved));
  ble_addr_t phone = {.type = BLE_ADDR_PUBLIC, .val = {4, 5, 6, 7, 8, 9}};
  cl_assert(!nimble_keyboard_owns_peer(&phone));
}

void test_keyboard_driver__failed_connection_clears_candidate_ownership(void) {
  nimble_keyboard_start();
  prv_drain();
  bt_keyboard_pair();
  prv_drain();
  prv_gap((struct ble_gap_event){
    .type = BLE_GAP_EVENT_DISC,
    .disc = {.event_type = BLE_HCI_ADV_RPT_EVTYPE_ADV_IND, .addr = s_saved}
  });
  cl_assert(nimble_keyboard_owns_peer(&s_saved));
  prv_gap((struct ble_gap_event){.type = BLE_GAP_EVENT_CONNECT, .connect.status = BLE_HS_ETIMEOUT});
  cl_assert(!nimble_keyboard_owns_peer(&s_saved));
}

void test_keyboard_driver__keyboard_bond_does_not_advertise_a_phone_gateway(void) {
  s_bonded = true;
  s_bond_peers[0] = s_saved;
  s_bond_peer_count = 1;
  cl_assert(!nimble_keyboard_has_gateway_bond());
  // Equal address bytes with a different address type represent a different peer.
  s_bond_peers[1] = s_saved;
  s_bond_peers[1].type = BLE_ADDR_PUBLIC;
  s_bond_peer_count = 2;
  s_conn_find_error = BLE_HS_ENOTCONN;
  cl_assert(nimble_keyboard_has_gateway_bond());
}

void test_keyboard_driver__uncommitted_resolved_keyboard_keys_are_not_a_gateway(void) {
  prv_start_pair();
  ble_addr_t identity = {.type = BLE_ADDR_PUBLIC, .val = {9, 8, 7, 6, 5, 4}};
  s_desc.peer_id_addr = identity;
  s_bond_peers[0] = identity;
  s_bond_peer_count = 1;
  cl_assert(!nimble_keyboard_has_gateway_bond());
  s_bond_peers[1] = (ble_addr_t){.type = BLE_ADDR_PUBLIC, .val = {1, 1, 1, 1, 1, 1}};
  s_bond_peer_count = 2;
  cl_assert(nimble_keyboard_has_gateway_bond());
}

void test_keyboard_driver__normal_low_power_l2cap_request_is_preserved(void) {
  prv_start_pair();
  struct ble_gap_upd_params request =
      {.itvl_min = 12, .itvl_max = 24, .latency = 99, .supervision_timeout = 1000};
  struct ble_gap_upd_params result = {0};
  prv_gap((struct ble_gap_event){
    .type = BLE_GAP_EVENT_L2CAP_UPDATE_REQ,
    .conn_update_req = {.conn_handle = 7, .peer_params = &request, .self_params = &result}
  });
  cl_assert_equal_m(&request, &result, sizeof(request));
}

void test_keyboard_driver__unchanged_status_does_not_send_another_event(void) {
  nimble_keyboard_start();
  prv_drain();
  prv_drain_kernel();
  int events = s_status_events;
  bt_keyboard_connect();
  prv_drain();
  prv_drain_kernel();
  cl_assert_equal_i(events, s_status_events);
}

void test_keyboard_driver__scan_timeout_is_expected_and_allows_another_pair(void) {
  nimble_keyboard_start();
  prv_drain();
  bt_keyboard_pair();
  prv_drain();
  prv_gap((struct ble_gap_event){.type = BLE_GAP_EVENT_DISC_COMPLETE});
  cl_assert_equal_i(BTKeyboardStateError, prv_status().state);
  cl_assert_equal_i(BLE_HS_ETIMEOUT, prv_status().error);
  cl_assert_equal_i(0, s_warning_count);
  cl_assert(!s_timer_active);
  bt_keyboard_pair();
  prv_drain();
  cl_assert_equal_i(BTKeyboardStateScanning, prv_status().state);
  cl_assert_equal_i(2, s_scan_count);
}

void test_keyboard_driver__scan_host_error_is_preserved_and_warns(void) {
  nimble_keyboard_start();
  prv_drain();
  bt_keyboard_pair();
  prv_drain();
  prv_gap((struct ble_gap_event){
    .type = BLE_GAP_EVENT_DISC_COMPLETE,
    .disc_complete.reason = BLE_HS_ECONTROLLER
  });
  cl_assert_equal_i(BTKeyboardStateError, prv_status().state);
  cl_assert_equal_i(BLE_HS_ECONTROLLER, prv_status().error);
  cl_assert_equal_i(1, s_warning_count);
  cl_assert(!s_timer_active);
}

void test_keyboard_driver__unowned_update_is_rejected_without_changing_parameters(void) {
  prv_start_pair();
  struct ble_gap_upd_params requested =
      {.itvl_min = 6, .itvl_max = 3200, .latency = 499, .supervision_timeout = 10};
  const struct ble_gap_upd_params original = {
    .itvl_min = 24,
    .itvl_max = 40,
    .supervision_timeout = 400
  };
  const uint8_t types[] = {BLE_GAP_EVENT_CONN_UPDATE_REQ, BLE_GAP_EVENT_L2CAP_UPDATE_REQ};
  for (size_t i = 0; i < sizeof(types); ++i) {
    struct ble_gap_upd_params result = original;
    struct ble_gap_event event = {
      .type = types[i],
      .conn_update_req = {.conn_handle = 8, .peer_params = &requested, .self_params = &result}
    };
    cl_assert_equal_i(BLE_ERR_CONN_PARMS, s_gap(&event, s_gap_arg));
    cl_assert_equal_m(&original, &result, sizeof(result));
    event.conn_update_req.conn_handle = 7;
    cl_assert_equal_i(BLE_ERR_CONN_PARMS, s_gap(&event, (void *)((uintptr_t)s_gap_arg - 1)));
    cl_assert_equal_m(&original, &result, sizeof(result));
  }
}
