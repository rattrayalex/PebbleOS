/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include "keyboard.h"
#include "window.h"

#include "applib/event_service_client.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/menu_layer.h"
#include <bluetooth/keyboard.h>
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "pbl/services/i18n/i18n.h"
#include "shell/system_theme.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  SettingsCallbacks callbacks;
  Window *window;
  EventServiceInfo status_event_info;
  BTKeyboardStatus status;
} SettingsKeyboardData;

enum {
  KeyboardRowStatus,
  KeyboardRowAction,
  KeyboardRowForget,
};

static bool prv_is_busy(const BTKeyboardStatus *status) {
  return status->state == BTKeyboardStateScanning || status->state == BTKeyboardStateConnecting ||
         status->state == BTKeyboardStateEncrypting || status->state == BTKeyboardStatePairing ||
         status->state == BTKeyboardStateDiscovering;
}

static const char *prv_status_text(const BTKeyboardStatus *status) {
  switch (status->state) {
    case BTKeyboardStateDisabled:
      return i18n_noop("Bluetooth is off");
    case BTKeyboardStateIdle:
      return status->bonded ? i18n_noop("Disconnected") : i18n_noop("Not paired");
    case BTKeyboardStateScanning:
      return i18n_noop("Searching...");
    case BTKeyboardStateConnecting:
    case BTKeyboardStateEncrypting:
      return i18n_noop("Connecting...");
    case BTKeyboardStatePairing:
      return i18n_noop("Pairing...");
    case BTKeyboardStateDiscovering:
      return i18n_noop("Setting up...");
    case BTKeyboardStateConnected:
      return i18n_noop("Connected");
    case BTKeyboardStateError:
      return i18n_noop("Could not connect");
  }
  return i18n_noop("Disconnected");
}

static const char *prv_action_text(const BTKeyboardStatus *status) {
  if (status->state == BTKeyboardStateDisabled) {
    return i18n_noop("Bluetooth is off");
  }
  if (prv_is_busy(status)) {
    return i18n_noop("Cancel");
  }
  if (status->state == BTKeyboardStateConnected) {
    return i18n_noop("Disconnect");
  }
  return status->bonded ? i18n_noop("Connect") : i18n_noop("Pair keyboard");
}

static const char *prv_instructions(const BTKeyboardStatus *status) {
  if (status->passkey_valid) {
    return i18n_noop("Type this code\non your keyboard.\nThen press Enter.\nSelect cancels.");
  }
  if (status->state == BTKeyboardStateDisabled) {
    return i18n_noop("Enable Bluetooth\nin Settings.");
  }
  if (!status->bonded && !prv_is_busy(status)) {
    return i18n_noop("Put only your\nkeyboard in\npairing mode.");
  }
  return NULL;
}

static void prv_refresh(SettingsKeyboardData *data) {
  BTKeyboardStatus status;
  bt_keyboard_get_status(&status);
  const BTKeyboardStatus *previous = &data->status;
  if (status.state == previous->state && status.bonded == previous->bonded &&
      status.passkey_valid == previous->passkey_valid && status.passkey == previous->passkey &&
      status.error == previous->error && strcmp(status.name, previous->name) == 0) {
    return;
  }
  data->status = status;
  settings_menu_reload_data(SettingsMenuItemKeyboard);
}

static void prv_appear_cb(SettingsCallbacks *context) {
  prv_refresh((SettingsKeyboardData *)context);
}

static void prv_status_event_handler(PebbleEvent *event, void *context) {
  SettingsKeyboardData *data = context;
  // The shared Settings reload helper addresses the current app-state window.
  // A covered window refreshes on appear instead of touching another submenu.
  if (app_window_stack_get_top_window() == data->window) {
    prv_refresh(data);
  }
}

static void prv_deinit_cb(SettingsCallbacks *context) {
  SettingsKeyboardData *data = (SettingsKeyboardData *)context;
  event_service_client_unsubscribe(&data->status_event_info);
  i18n_free_all(data);
  app_free(data);
}

static uint16_t prv_initial_selection_cb(SettingsCallbacks *context) {
  return KeyboardRowAction;
}

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  SettingsKeyboardData *data = (SettingsKeyboardData *)context;
  return data->status.bonded ? 3 : 2;
}

static int16_t prv_row_height_cb(SettingsCallbacks *context, uint16_t row, bool is_selected) {
  SettingsKeyboardData *data = (SettingsKeyboardData *)context;
  if (row == KeyboardRowAction && prv_instructions(&data->status) &&
      PBL_IF_RECT_ELSE(true, is_selected)) {
    const GFont title_font = system_theme_get_font(TextStyleFont_MenuCellTitle);
    const GFont subtitle_font = system_theme_get_font(TextStyleFont_Footer);
    return fonts_get_font_height(title_font) + 4 * fonts_get_font_height(subtitle_font) + 10;
  }
  return PBL_IF_RECT_ELSE(menu_cell_basic_cell_height(),
                          (is_selected ? MENU_CELL_ROUND_FOCUSED_TALL_CELL_HEIGHT
                                       : MENU_CELL_ROUND_UNFOCUSED_SHORT_CELL_HEIGHT));
}

static void prv_draw_instructions(SettingsKeyboardData *data, GContext *ctx,
                                  const Layer *cell_layer, const char *title,
                                  const char *instructions) {
  const GFont title_font = system_theme_get_font(TextStyleFont_MenuCellTitle);
  const GFont subtitle_font = system_theme_get_font(TextStyleFont_Footer);
  const int16_t title_height = fonts_get_font_height(title_font);
  GRect box = grect_inset(cell_layer->bounds, GEdgeInsets(4, PBL_IF_RECT_ELSE(5, 20)));
  box.size.h = title_height + 4;
  graphics_draw_text(ctx, title, title_font, box, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
  box.origin.y += title_height + 2;
  box.size.h = cell_layer->bounds.size.h - box.origin.y - 4;
  graphics_draw_text(ctx, i18n_get(instructions, data), subtitle_font, box,
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx, const Layer *cell_layer,
                            uint16_t row, bool selected) {
  SettingsKeyboardData *data = (SettingsKeyboardData *)context;
  const BTKeyboardStatus *status = &data->status;
  if (row == KeyboardRowStatus) {
    const char *name = status->name[0] ? status->name : i18n_get("Keyboard", data);
    menu_cell_basic_draw(ctx, cell_layer, name, i18n_get(prv_status_text(status), data), NULL);
  } else if (row == KeyboardRowAction) {
    char passkey[16];
    const char *title = i18n_get(prv_action_text(status), data);
    if (status->passkey_valid) {
      snprintf(passkey, sizeof(passkey), "%06lu", (unsigned long)status->passkey);
      title = passkey;
    }
    const char *instructions = prv_instructions(status);
    if (instructions && PBL_IF_RECT_ELSE(true, selected)) {
      prv_draw_instructions(data, ctx, cell_layer, title, instructions);
    } else {
      menu_cell_basic_draw(ctx, cell_layer, title, NULL, NULL);
    }
  } else if (row == KeyboardRowForget) {
    menu_cell_basic_draw(ctx, cell_layer, i18n_get("Forget", data), NULL, NULL);
  }
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  SettingsKeyboardData *data = (SettingsKeyboardData *)context;
  if (row == KeyboardRowForget && data->status.bonded) {
    bt_keyboard_forget();
  } else if (row == KeyboardRowAction) {
    if (data->status.state == BTKeyboardStateDisabled) {
      return;
    }
    if (prv_is_busy(&data->status) || data->status.state == BTKeyboardStateConnected) {
      bt_keyboard_disconnect();
    } else if (data->status.bonded) {
      bt_keyboard_connect();
    } else {
      bt_keyboard_pair();
    }
  }
  prv_refresh(data);
}

static Window *prv_init(void) {
  SettingsKeyboardData *data = app_zalloc_check(sizeof(*data));
  data->callbacks = (SettingsCallbacks){
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .get_initial_selection = prv_initial_selection_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
    .row_height = prv_row_height_cb,
    .appear = prv_appear_cb,
  };
  bt_keyboard_get_status(&data->status);
  data->window = settings_window_create(SettingsMenuItemKeyboard, &data->callbacks);
  data->status_event_info = (EventServiceInfo){
    .type = PEBBLE_BT_KEYBOARD_STATUS_CHANGED_EVENT,
    .handler = prv_status_event_handler,
    .context = data,
  };
  event_service_client_subscribe(&data->status_event_info);
  return data->window;
}

const SettingsModuleMetadata *settings_keyboard_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    .name = i18n_noop("Keyboard"),
    .init = prv_init,
  };
  return &s_module_info;
}
