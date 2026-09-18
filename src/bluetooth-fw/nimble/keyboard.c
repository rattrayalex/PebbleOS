/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include "keyboard_internal.h"

#include <bluetooth/keyboard.h>
#include <host/ble_hs.h>
#include <host/ble_sm.h>
#include <kernel/pbl_malloc.h>
#include <nimble/nimble_port.h>
#include <os/os_mbuf.h>
#include <pbl/kernel/mutex.h>
#include <pbl/kernel/sem.h>
#include <system/passert.h>
#include <pbl/logging/logging.h>
#include <string.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

#if !MYNEWT_VAL(BLE_SM_SC_ONLY) || !MYNEWT_VAL(BLE_SM_MITM)
#error "Keyboard input requires authenticated LE Secure Connections"
#endif

#define HID_SERVICE_UUID    0x1812
#define BOOT_INPUT_UUID     0x2a22
#define PROTOCOL_MODE_UUID  0x2a4e
#define CCCD_UUID           0x2902
#define KEYBOARD_APPEARANCE 0x03c1
#define SCAN_TIMEOUT_MS     30000
#define CONNECT_TIMEOUT_MS  10000
#define RECONNECT_DELAY_MS  60000

static const ble_uuid16_t s_hid_uuid = BLE_UUID16_INIT(HID_SERVICE_UUID);
static PBL_MUTEX_DEFINE(s_status_mutex);
static PBL_SEM_DEFINE(s_prepared, 0, 1);
static BTKeyboardStatus s_status = {.state = BTKeyboardStateDisabled};
static BTKeyboardStatus s_public_status = {.state = BTKeyboardStateDisabled};
static bool s_allowed;
static uint32_t s_request_epoch;
static void (*s_stop_complete)(void *);
static void *s_stop_context;
static bool s_reconnect;
static bool s_restore_irk_pending;
static struct ble_npl_callout s_retry;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_generation;
static bool s_scanning;
static bool s_connecting;
static bool s_pair_requested;
static bool s_forget_pending;
static bool s_have_peer;
static ble_addr_t s_peer;
static uint16_t s_service_start;
static uint16_t s_service_end;
static uint16_t s_input;
static uint16_t s_input_end;
static uint16_t s_protocol;
static uint16_t s_cccd;
static ble_addr_t s_advertisers[4];
static unsigned s_advertiser_count;
static unsigned s_advertiser_next;

// Each request owns its event, so consecutive commands cannot overwrite each other.
typedef enum {
  KeyboardCommandPrepare,
  KeyboardCommandStart,
  KeyboardCommandStopped,
  KeyboardCommandReconnect,
  KeyboardCommandPair,
  KeyboardCommandConnect,
  KeyboardCommandDisconnect,
  KeyboardCommandForget,
} KeyboardCommand;

typedef struct {
  struct ble_npl_event event;
  KeyboardCommand command;
  uint32_t epoch;
} KeyboardRequest;

static int prv_gap_event(struct ble_gap_event *event, void *arg);
static void prv_request(KeyboardCommand command);

static bool prv_allowed(void) {
  pbl_mutex_lock(&s_status_mutex, PBL_FOREVER);
  bool allowed = s_allowed;
  pbl_mutex_unlock(&s_status_mutex);
  return allowed;
}

static void prv_publish(BTKeyboardState state, int error) {
  s_status.state = state;
  s_status.error = error;
  s_status.bonded = nimble_keyboard_store_get(NULL, NULL);
  if (state != BTKeyboardStatePairing) {
    s_status.passkey_valid = false;
    s_status.passkey = 0;
  }
  pbl_mutex_lock(&s_status_mutex, PBL_FOREVER);
  s_public_status = s_status;
  pbl_mutex_unlock(&s_status_mutex);
}

void bt_keyboard_get_status(BTKeyboardStatus *out) {
  pbl_mutex_lock(&s_status_mutex, PBL_FOREVER);
  *out = s_public_status;
  if (!s_allowed) {
    out->state = BTKeyboardStateDisabled;
    out->passkey_valid = false;
    out->passkey = 0;
  }
  pbl_mutex_unlock(&s_status_mutex);
}

static void prv_cancel(void) {
  ble_npl_callout_stop(&s_retry);
  ++s_generation;
  // Clear flags before cancelling: cancellation can deliver events synchronously.
  if (s_scanning) {
    s_scanning = false;
    ble_gap_disc_cancel();
  }
  if (s_connecting) {
    s_connecting = false;
    ble_gap_conn_cancel();
  }
  bt_keyboard_release_buttons();
  if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
  }
}

static void prv_fail(int error) {
  s_reconnect = false;
  prv_publish(BTKeyboardStateError, error);
  prv_cancel();
}

static void prv_retry_later(void) {
  if (s_reconnect && prv_allowed() && nimble_keyboard_store_get(NULL, NULL)) {
    ble_npl_time_t ticks;
    ble_npl_time_ms_to_ticks(RECONNECT_DELAY_MS, &ticks);
    ble_npl_callout_reset(&s_retry, ticks);
  }
}

static void prv_connection_failed(int error) {
  s_have_peer = false;
  bool reconnect = s_reconnect;
  prv_fail(error);
  s_reconnect = reconnect;
  prv_retry_later();
}

static bool prv_current(uint16_t conn, void *arg) {
  return conn == s_conn && (uint32_t)(uintptr_t)arg == s_generation && prv_allowed();
}

bool nimble_keyboard_owns_peer(const ble_addr_t *peer) {
  if (nimble_keyboard_store_matches(peer) || (s_have_peer && ble_addr_cmp(peer, &s_peer) == 0)) {
    return true;
  }
  struct ble_gap_conn_desc desc;
  return s_conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(s_conn, &desc) == 0 &&
         (ble_addr_cmp(peer, &desc.peer_id_addr) == 0 ||
          ble_addr_cmp(peer, &desc.peer_ota_addr) == 0);
}

static void prv_forget(void) {
  ble_addr_t peer;
  if (nimble_keyboard_store_get(&peer, NULL)) {
    // Remove controller and in-memory keys while this is still classified as an accessory.
    int rc = ble_hs_is_enabled() ? ble_gap_unpair(&peer) : ble_store_util_delete_peer(&peer);
    if (rc) {
      s_forget_pending = false;
      prv_publish(BTKeyboardStateError, rc);
      return;
    }
  }
  int rc = nimble_keyboard_store_forget();
  if (rc) {
    // A failed flash write must leave the existing bond usable in this session.
    nimble_keyboard_store_restore_keys();
    s_restore_irk_pending = ble_hs_is_enabled() && nimble_keyboard_store_restore_irk() != 0;
  }
  s_forget_pending = false;
  s_have_peer = false;
  if (rc) {
    prv_publish(BTKeyboardStateError, rc);
    return;
  }
  s_restore_irk_pending = false;
  memset(s_status.name, 0, sizeof(s_status.name));
  prv_publish(prv_allowed() ? BTKeyboardStateIdle : BTKeyboardStateDisabled, 0);
}

static int prv_subscribed(uint16_t conn, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg) {
  if (!prv_current(conn, arg)) {
    return 0;
  }
  if (error->status) {
    prv_fail(error->status);
    return 0;
  }
  int rc = nimble_keyboard_store_commit(s_status.name);
  if (rc) {
    prv_fail(rc);
    return 0;
  }
  if (s_pair_requested) {
    s_restore_irk_pending = false;
    bt_keyboard_suppress_pairing_enter();
  }
  s_pair_requested = false;
  s_reconnect = true;
  prv_publish(BTKeyboardStateConnected, 0);
  return 0;
}

static int prv_descriptor(uint16_t conn, const struct ble_gatt_error *error,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  if (!prv_current(conn, arg)) {
    return 0;
  }
  if (error->status == 0) {
    if (ble_uuid_u16(&dsc->uuid.u) == CCCD_UUID) {
      s_cccd = dsc->handle;
    }
    return 0;
  }
  if (error->status != BLE_HS_EDONE || !s_cccd) {
    prv_fail(error->status == BLE_HS_EDONE ? BLE_HS_ENOTSUP : error->status);
    return 0;
  }
  const uint8_t boot_mode = 0;
  // HID Protocol Mode requires Write Without Response. ATT preserves ordering
  // with the subsequent acknowledged CCCD write on this connection.
  int rc = ble_gattc_write_no_rsp_flat(conn, s_protocol, &boot_mode, sizeof(boot_mode));
  if (!rc) {
    const uint8_t notifications[] = {1, 0};
    rc = ble_gattc_write_flat(conn, s_cccd, notifications, sizeof(notifications), prv_subscribed,
                              arg);
  }
  if (rc) {
    prv_fail(rc);
  }
  return 0;
}

static int prv_characteristic(uint16_t conn, const struct ble_gatt_error *error,
                              const struct ble_gatt_chr *chr, void *arg) {
  if (!prv_current(conn, arg)) {
    return 0;
  }
  if (!error->status) {
    // Bound descriptor discovery to the boot input characteristic, not the service.
    if (s_input && !s_input_end && chr->def_handle > s_input) {
      s_input_end = chr->def_handle - 1;
    }
    uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
    if (uuid == BOOT_INPUT_UUID && !s_input && (chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) {
      s_input = chr->val_handle;
    } else if (uuid == PROTOCOL_MODE_UUID && (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP)) {
      s_protocol = chr->val_handle;
    }
    return 0;
  }
  if (error->status != BLE_HS_EDONE || !s_input || !s_protocol) {
    prv_fail(error->status == BLE_HS_EDONE ? BLE_HS_ENOTSUP : error->status);
    return 0;
  }
  if (!s_input_end) {
    s_input_end = s_service_end;
  }
  int rc = ble_gattc_disc_all_dscs(conn, s_input, s_input_end, prv_descriptor, arg);
  if (rc) {
    prv_fail(rc);
  }
  return 0;
}

static int prv_service(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg) {
  if (!prv_current(conn, arg)) {
    return 0;
  }
  if (!error->status) {
    if (!s_service_start) {
      s_service_start = service->start_handle;
      s_service_end = service->end_handle;
    }
    return 0;
  }
  if (error->status != BLE_HS_EDONE || !s_service_start) {
    prv_fail(error->status == BLE_HS_EDONE ? BLE_HS_ENOTSUP : error->status);
    return 0;
  }
  int rc = ble_gattc_disc_all_chrs(conn, s_service_start, s_service_end, prv_characteristic, arg);
  if (rc) {
    prv_fail(rc);
  }
  return 0;
}

static void prv_encrypted(int status) {
  struct ble_gap_conn_desc desc;
  struct ble_store_key_sec key = {0};
  struct ble_store_value_sec security;
  int rc = status ? status : ble_gap_conn_find(s_conn, &desc);
  if (!rc && (!desc.sec_state.encrypted || !desc.sec_state.authenticated ||
              !desc.sec_state.bonded || desc.sec_state.key_size != 16)) {
    rc = BLE_HS_EAUTHEN;
  }
  if (!rc) {
    key.peer_addr = desc.peer_id_addr;
    rc = ble_store_read_peer_sec(&key, &security);
    if (!rc && (!security.sc || !security.authenticated || !security.ltk_present)) {
      rc = BLE_HS_EAUTHEN;
    }
  }
  if (rc) {
    prv_fail(rc);
    return;
  }
  if (s_status.state != BTKeyboardStatePairing) {
    return;
  }
  s_peer = desc.peer_id_addr;
  s_have_peer = true;
  s_service_start = s_service_end = s_input = s_input_end = s_protocol = s_cccd = 0;
  prv_publish(BTKeyboardStateDiscovering, 0);
  rc = ble_gattc_disc_svc_by_uuid(s_conn, &s_hid_uuid.u, prv_service,
                                  (void *)(uintptr_t)s_generation);
  if (rc) {
    prv_fail(rc);
  }
}

static void prv_connect(const ble_addr_t *peer) {
  uint8_t own_type;
  int rc = ble_hs_id_infer_auto(0, &own_type);
  if (rc) {
    prv_fail(rc);
    return;
  }
  s_peer = *peer;
  s_peer.type &= 1; // Remember an identity in the format used by the security store.
  s_have_peer = true;
  s_connecting = true;
  prv_publish(BTKeyboardStateConnecting, 0);
  const struct ble_gap_conn_params params = {
    .scan_itvl = 0x100, // 160 ms, with a 10 ms window to limit background power use.
    .scan_window = 0x10,
    .itvl_min = 24,
    .itvl_max = 40,
    .supervision_timeout = 400,
  };
  rc = ble_gap_connect(own_type, peer, CONNECT_TIMEOUT_MS, &params, prv_gap_event,
                       (void *)(uintptr_t)s_generation);
  if (rc) {
    s_connecting = false;
    prv_connection_failed(rc);
  }
}

static void prv_advertisement(const struct ble_gap_disc_desc *disc) {
  if (!s_scanning) {
    return;
  }
  if (disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND) {
    s_advertisers[s_advertiser_next] = disc->addr;
    s_advertiser_next = (s_advertiser_next + 1) % 4;
    if (s_advertiser_count < 4) {
      ++s_advertiser_count;
    }
  } else if (disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
    bool connectable = false;
    for (unsigned i = 0; i < s_advertiser_count; ++i) {
      connectable |= ble_addr_cmp(&disc->addr, &s_advertisers[i]) == 0;
    }
    if (!connectable) {
      return;
    }
  } else {
    return;
  }
  struct ble_hs_adv_fields fields;
  if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data)) {
    return;
  }
  bool hid = fields.appearance_is_present && fields.appearance == KEYBOARD_APPEARANCE;
  for (unsigned i = 0; i < fields.num_uuids16; ++i) {
    hid |= ble_uuid_u16(&fields.uuids16[i].u) == HID_SERVICE_UUID;
  }
  if (!hid) {
    return;
  }
  memset(s_status.name, 0, sizeof(s_status.name));
  if (fields.name && fields.name_len) {
    size_t len = fields.name_len;
    if (len >= sizeof(s_status.name)) {
      len = sizeof(s_status.name) - 1;
    }
    // Advertisement names are untrusted: keep a single printable Settings line.
    for (size_t i = 0; i < len; ++i) {
      uint8_t c = fields.name[i];
      s_status.name[i] = (c >= 0x20 && c <= 0x7e) ? c : '?';
    }
  } else {
    strcpy(s_status.name, "BLE keyboard");
  }
  s_scanning = false;
  int rc = ble_gap_disc_cancel();
  if (rc) {
    prv_fail(rc);
    return;
  }
  prv_connect(&disc->addr);
}

static void prv_passkey(const struct ble_gap_event *event) {
  // Never silently repair a lost bond or accept numeric comparison / Just Works.
  if (!s_pair_requested || event->passkey.params.action != BLE_SM_IOACT_DISP) {
    prv_fail(BLE_HS_EAUTHEN);
    return;
  }
  struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP};
  int rc;
  do {
    rc = ble_hs_hci_rand(&io.passkey, sizeof(io.passkey));
  } while (!rc && io.passkey >= 4294000000UL);
  if (rc) {
    prv_fail(rc);
    return;
  }
  io.passkey %= 1000000;
  s_status.passkey = io.passkey;
  s_status.passkey_valid = true;
  prv_publish(BTKeyboardStatePairing, 0);
  rc = ble_sm_inject_io(s_conn, &io);
  if (rc) {
    prv_fail(rc);
  }
}

static int prv_gap_event(struct ble_gap_event *event, void *arg) {
  bool current = (uint32_t)(uintptr_t)arg == s_generation && prv_allowed();
  switch (event->type) {
    case BLE_GAP_EVENT_DISC:
      if (current) {
        prv_advertisement(&event->disc);
      }
      break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
      if (current && s_scanning) {
        s_scanning = false;
        prv_fail(BLE_HS_ETIMEOUT);
      }
      break;
    case BLE_GAP_EVENT_CONNECT:
      if (!current) {
        if (!event->connect.status) {
          ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;
      }
      s_connecting = false;
      if (event->connect.status) {
        prv_connection_failed(event->connect.status);
        break;
      }
      s_conn = event->connect.conn_handle;
      prv_publish(BTKeyboardStatePairing, 0);
      {
        // Only explicit Pair can generate new keys. Reconnect requires a stored LTK.
        int rc = ble_gap_security_initiate(s_conn);
        if (rc) {
          prv_fail(rc);
        }
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      if (event->disconnect.conn.conn_handle != s_conn) {
        break;
      }
      bt_keyboard_release_buttons();
      s_conn = BLE_HS_CONN_HANDLE_NONE;
      ++s_generation;
      if (s_pair_requested && !nimble_keyboard_store_get(NULL, NULL)) {
        // Failed/unsupported pairing must not leave transient keys as phone bonds.
        int rc = ble_gap_unpair(&event->disconnect.conn.peer_id_addr);
        if (rc) {
          // The host may already be stopping; still erase transient host keys.
          ble_store_util_delete_peer(&event->disconnect.conn.peer_id_addr);
        }
        nimble_keyboard_store_clear_pending();
      }
      s_pair_requested = false;
      if (s_forget_pending) {
        prv_forget();
      } else if (!prv_allowed()) {
        prv_publish(BTKeyboardStateDisabled, 0);
      } else if (s_status.state != BTKeyboardStateError) {
        prv_publish(BTKeyboardStateIdle, 0);
      }
      s_have_peer = false;
      prv_retry_later();
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (current && event->enc_change.conn_handle == s_conn) {
        prv_encrypted(event->enc_change.status);
      }
      break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      if (current && event->passkey.conn_handle == s_conn) {
        prv_passkey(event);
      }
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_PAIRING_COMPLETE:
      if (current && event->pairing_complete.status) {
        prv_fail(event->pairing_complete.status);
      }
      break;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
      if (current) {
        struct ble_gap_conn_desc desc;
        if (!ble_gap_conn_find(s_conn, &desc)) {
          s_peer = desc.peer_id_addr;
        }
      }
      break;
    case BLE_GAP_EVENT_NOTIFY_RX:
      if (current && event->notify_rx.conn_handle == s_conn &&
          s_status.state == BTKeyboardStateConnected && event->notify_rx.attr_handle == s_input) {
        uint8_t report[8];
        size_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (length == sizeof(report) && !os_mbuf_copydata(event->notify_rx.om, 0, length, report)) {
          bt_keyboard_handle_report(report, length);
        } else {
          bt_keyboard_release_buttons();
        }
      }
      break;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
      *event->conn_update_req.self_params = *event->conn_update_req.peer_params;
      break;
    default:
      break;
  }
  return 0;
}

static void prv_command(struct ble_npl_event *event) {
  KeyboardRequest *request = ble_npl_event_get_arg(event);
  KeyboardCommand command = request->command;
  pbl_mutex_lock(&s_status_mutex, PBL_FOREVER);
  bool current = request->epoch == s_request_epoch;
  pbl_mutex_unlock(&s_status_mutex);
  kernel_free(request);
  if (command == KeyboardCommandPrepare) {
    nimble_keyboard_store_load();
    nimble_keyboard_store_get(NULL, s_status.name);
    prv_publish(BTKeyboardStateDisabled, 0);
    pbl_sem_give(&s_prepared);
    return;
  }
  if (!current) {
    return;
  }
  if (command == KeyboardCommandStopped) {
    ++s_generation;
    ble_npl_callout_stop(&s_retry);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_scanning = s_connecting = s_pair_requested = s_have_peer = false;
    s_forget_pending = false;
    s_restore_irk_pending = false;
    bt_keyboard_release_buttons();
    nimble_keyboard_store_clear_pending();
    prv_publish(BTKeyboardStateDisabled, 0);
    pbl_mutex_lock(&s_status_mutex, PBL_FOREVER);
    void (*complete)(void *) = s_stop_complete;
    void *context = s_stop_context;
    s_stop_complete = NULL;
    s_stop_context = NULL;
    pbl_mutex_unlock(&s_status_mutex);
    if (complete) {
      complete(context);
    }
    return;
  }
  if (!prv_allowed()) {
    if (command == KeyboardCommandForget) {
      prv_forget();
    }
    return;
  }
  if (command == KeyboardCommandDisconnect || command == KeyboardCommandForget) {
    s_reconnect = false;
    s_forget_pending |= command == KeyboardCommandForget;
    prv_publish(BTKeyboardStateIdle, 0);
    prv_cancel();
    if (s_forget_pending && s_conn == BLE_HS_CONN_HANDLE_NONE) {
      prv_forget();
    }
    return;
  }
  if (s_conn != BLE_HS_CONN_HANDLE_NONE || s_scanning || s_connecting) {
    return;
  }
  if (command == KeyboardCommandReconnect && !s_reconnect) {
    return;
  }
  ble_npl_callout_stop(&s_retry);
  ++s_generation;
  if (command == KeyboardCommandStart) {
    prv_publish(BTKeyboardStateIdle, 0);
  }
  ble_addr_t peer;
  bool bonded = nimble_keyboard_store_get(&peer, s_status.name);
  if (command == KeyboardCommandPair) {
    if (bonded) {
      return;
    }
    s_reconnect = false;
    s_pair_requested = true;
    s_advertiser_count = s_advertiser_next = 0;
    nimble_keyboard_store_clear_pending();
    uint8_t own_type;
    int rc = ble_hs_id_infer_auto(0, &own_type);
    struct ble_gap_disc_params params = {
      .itvl = 0x80,
      .window = 0x40,
      .passive = 0,
      .filter_duplicates = 1,
    };
    if (!rc) {
      s_scanning = true;
      prv_publish(BTKeyboardStateScanning, 0);
      rc = ble_gap_disc(own_type, SCAN_TIMEOUT_MS, &params, prv_gap_event,
                        (void *)(uintptr_t)s_generation);
    }
    if (rc) {
      s_scanning = false;
      prv_fail(rc);
    }
  } else if (bonded) {
    s_pair_requested = false;
    s_reconnect = true;
    if (s_restore_irk_pending) {
      int rc = nimble_keyboard_store_restore_irk();
      if (rc) {
        prv_connection_failed(rc);
        return;
      }
      s_restore_irk_pending = false;
    }
    if (nimble_keyboard_store_get_connection_addr(&peer)) {
      prv_connect(&peer);
    }
  } else {
    prv_publish(BTKeyboardStateIdle, 0);
  }
}

static void prv_request(KeyboardCommand command) {
  pbl_mutex_lock(&s_status_mutex, PBL_FOREVER);
  bool allowed = s_allowed || command == KeyboardCommandPrepare ||
                 command == KeyboardCommandStopped || command == KeyboardCommandForget;
  uint32_t epoch = s_request_epoch;
  pbl_mutex_unlock(&s_status_mutex);
  if (!allowed) {
    return;
  }
  KeyboardRequest *request = kernel_malloc_check(sizeof(*request));
  request->command = command;
  request->epoch = epoch;
  ble_npl_event_init(&request->event, prv_command, request);
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &request->event);
}

void bt_keyboard_pair(void) {
  prv_request(KeyboardCommandPair);
}
void bt_keyboard_connect(void) {
  prv_request(KeyboardCommandConnect);
}
void bt_keyboard_disconnect(void) {
  prv_request(KeyboardCommandDisconnect);
}
void bt_keyboard_forget(void) {
  prv_request(KeyboardCommandForget);
}

static void prv_retry(struct ble_npl_event *event) {
  if (s_reconnect) {
    prv_request(KeyboardCommandReconnect);
  }
}

void nimble_keyboard_init(void) {
  ble_npl_callout_init(&s_retry, nimble_port_get_dflt_eventq(), prv_retry, NULL);
}

void nimble_keyboard_prepare(void) {
  // Storage stays on the host thread, including Forget while Bluetooth is off.
  prv_request(KeyboardCommandPrepare);
  PBL_ASSERTN(pbl_sem_take(&s_prepared, PBL_MSEC(10000)) == 0);
}

void nimble_keyboard_start(void) {
  pbl_mutex_lock(&s_status_mutex, PBL_FOREVER);
  s_allowed = true;
  pbl_mutex_unlock(&s_status_mutex);
  prv_request(KeyboardCommandStart);
}

void nimble_keyboard_disallow(void) {
  pbl_mutex_lock(&s_status_mutex, PBL_FOREVER);
  s_allowed = false;
  pbl_mutex_unlock(&s_status_mutex);
}

void nimble_keyboard_stopped(void (*complete)(void *), void *context) {
  pbl_mutex_lock(&s_status_mutex, PBL_FOREVER);
  if (complete) {
    s_stop_complete = complete;
    s_stop_context = context;
  }
  ++s_request_epoch;
  pbl_mutex_unlock(&s_status_mutex);
  // NimBLE can complete stop synchronously on its caller when no links remain.
  // Always defer cleanup so button releases run on the same thread as reports.
  prv_request(KeyboardCommandStopped);
}

void nimble_keyboard_resynced(void) {
  if (prv_allowed()) {
    prv_request(KeyboardCommandStart);
  }
}
