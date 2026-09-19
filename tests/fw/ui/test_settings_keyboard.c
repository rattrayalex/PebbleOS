/* SPDX-FileCopyrightText: 2026 Alex Rattray */
/* SPDX-License-Identifier: Apache-2.0 */

#include <bluetooth/keyboard.h>
#include "applib/app_timer.h"
#include "applib/event_service_client.h"
#include "applib/graphics/framebuffer.h"
#include "applib/graphics/graphics.h"
#include "applib/ui/app_window_stack.h"
#include "resource/resource.h"
#include "shell/system_theme.h"
#include "system/passert.h"
#include "pbl/util/size.h"
#include "clar.h"

static GContext s_ctx;
static Window *s_top_window;
static BTKeyboardStatus s_status;
static unsigned int s_status_reads;
static unsigned int s_timer_registrations;
static unsigned int s_pair_requests;
static unsigned int s_connect_requests;
static unsigned int s_disconnect_requests;
static unsigned int s_forget_requests;
static EventServiceInfo *s_events[8];

#include "fake_content_indicator.h"
#include "fake_spi_flash.h"
#include "../../fixtures/load_test_resources.h"
#include "stubs_analytics.h"
#include "stubs_app_install_manager.h"
#include "stubs_app_state.h"
#include "stubs_bootbits.h"
#include "stubs_buffer.h"
#include "stubs_click.h"
#include "stubs_heap.h"
#include "stubs_i18n.h"
#include "stubs_layer.h"
#include "stubs_logging.h"
#include "stubs_memory_layout.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_print.h"
#include "stubs_process_manager.h"
#include "stubs_prompt.h"
#include "stubs_serial.h"
#include "stubs_shell_prefs.h"
#include "stubs_sleep.h"
#include "stubs_syscall_internal.h"
#include "stubs_syscalls.h"
#include "stubs_task_watchdog.h"
#include "stubs_vibes.h"
#include "stubs_window_manager.h"
#include "stubs_window_stack.h"

GColor shell_prefs_get_theme_highlight_color(void) {
  return GColorVividCerulean;
}

GContext *graphics_context_get_current_context(void) {
  return &s_ctx;
}

Window *app_window_stack_get_top_window(void) {
  return s_top_window;
}

AppTimer *app_timer_register(uint32_t timeout_ms, AppTimerCallback callback, void *context) {
  ++s_timer_registrations;
  return NULL;
}

AppTimer *app_timer_register_repeatable(uint32_t timeout_ms, AppTimerCallback callback,
                                        void *context, bool repeating) {
  ++s_timer_registrations;
  return NULL;
}

bool app_timer_reschedule(AppTimer *timer, uint32_t timeout_ms) {
  return true;
}

void app_timer_cancel(AppTimer *timer) {
}

void event_service_client_subscribe(EventServiceInfo *info) {
  for (unsigned int i = 0; i < ARRAY_LENGTH(s_events); ++i) {
    if (!s_events[i]) {
      s_events[i] = info;
      return;
    }
  }
  cl_assert(false);
}

void event_service_client_unsubscribe(EventServiceInfo *info) {
  for (unsigned int i = 0; i < ARRAY_LENGTH(s_events); ++i) {
    if (s_events[i] == info) {
      s_events[i] = NULL;
      return;
    }
  }
  cl_assert(false);
}

void bt_keyboard_get_status(BTKeyboardStatus *status) {
  ++s_status_reads;
  *status = s_status;
}

void bt_keyboard_pair(void) {
  ++s_pair_requests;
}
void bt_keyboard_connect(void) {
  ++s_connect_requests;
}
void bt_keyboard_disconnect(void) {
  ++s_disconnect_requests;
}
void bt_keyboard_forget(void) {
  ++s_forget_requests;
}

int16_t interpolate_int16(int32_t normalized, int16_t from, int16_t to) {
  return to;
}
AnimationProgress animation_timing_scaled(AnimationProgress time, AnimationProgress start,
                                          AnimationProgress end) {
  return end;
}
int64_t interpolate_moook(int32_t normalized, int64_t from, int64_t to) {
  return to;
}
uint32_t interpolate_moook_duration(void) {
  return 0;
}

// Render the actual Settings window and keyboard callbacks, with only platform services faked.
#include "apps/system/settings/window.c"
#include "apps/system/settings/keyboard.c"

const SettingsModuleMetadata *settings_menu_get_submodule_info(SettingsMenuItem category) {
  cl_assert_equal_i(category, SettingsMenuItemKeyboard);
  return settings_keyboard_get_info();
}

const char *settings_menu_get_status_name(SettingsMenuItem category) {
  return settings_menu_get_submodule_info(category)->name;
}

#include "../graphics/test_graphics.h"
#include "../graphics/util.h"

static FrameBuffer *s_framebuffer;

void test_settings_keyboard__initialize(void) {
  s_framebuffer = malloc(sizeof(*s_framebuffer));
  framebuffer_init(s_framebuffer, &(GSize){DISP_COLS, DISP_ROWS});
  test_graphics_context_init(&s_ctx, s_framebuffer);
  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);
  pfs_format(true);
  load_resource_fixture_in_flash(RESOURCES_FIXTURE_PATH, SYSTEM_RESOURCES_FIXTURE_NAME, false);
  resource_init();
  memset(s_events, 0, sizeof(s_events));
  s_status = (BTKeyboardStatus){.state = BTKeyboardStateIdle};
  s_status_reads = s_timer_registrations = 0;
  s_pair_requests = s_connect_requests = s_disconnect_requests = s_forget_requests = 0;
  app_state_set_user_data(NULL);
}

static Window *prv_open_keyboard(void) {
  Window *window = settings_keyboard_get_info()->init();
  s_top_window = window;
  window_set_on_screen(window, true, true);
  return window;
}

static void prv_close_keyboard(Window *window) {
  window_set_on_screen(window, false, true);
  window_unload(window);
  s_top_window = NULL;
}

void test_settings_keyboard__cleanup(void) {
  for (unsigned int i = 0; i < ARRAY_LENGTH(s_events); ++i) {
    cl_assert_equal_p(s_events[i], NULL);
  }
  cl_assert_equal_p(app_state_get_user_data(), NULL);
  free(s_framebuffer);
}

void test_settings_keyboard__does_not_poll(void) {
  Window *window = prv_open_keyboard();
  const unsigned int timers = s_timer_registrations;
  prv_close_keyboard(window);
  cl_assert_equal_i(timers, 0);
}

static void prv_send_status_event(void) {
  PebbleEvent event = {.type = PEBBLE_BT_KEYBOARD_STATUS_CHANGED_EVENT};
  for (unsigned int i = 0; i < ARRAY_LENGTH(s_events); ++i) {
    EventServiceInfo *info = s_events[i];
    if (info && info->type == event.type) {
      info->handler(&event, info->context);
    }
  }
}

void test_settings_keyboard__status_events_refresh_visible_window_and_unsubscribe(void) {
  Window *window = prv_open_keyboard();
  SettingsData *settings = window_get_user_data(window);
  SettingsKeyboardData *keyboard = (SettingsKeyboardData *)settings->callbacks;
  unsigned int reads = s_status_reads;
  s_status =
      (BTKeyboardStatus){.state = BTKeyboardStatePairing, .passkey_valid = true, .passkey = 12345};
  prv_send_status_event();
  cl_assert_equal_i(s_status_reads, reads + 1);
  cl_assert_equal_i(keyboard->status.passkey, 12345);
  cl_assert(keyboard->status.passkey_valid);

  s_status = (BTKeyboardStatus){
    .state = BTKeyboardStateConnected,
    .bonded = true,
    .name = "Test keyboard"
  };
  prv_send_status_event();
  cl_assert_equal_i(keyboard->status.state, BTKeyboardStateConnected);
  cl_assert_equal_i(keyboard->callbacks.num_rows(&keyboard->callbacks), 3);
  cl_assert_equal_s(keyboard->status.name, "Test keyboard");
  prv_close_keyboard(window);
  reads = s_status_reads;
  prv_send_status_event();
  cl_assert_equal_i(s_status_reads, reads);
}

void test_settings_keyboard__covered_window_defers_refresh_until_appear(void) {
  Window *window = prv_open_keyboard();
  SettingsData *settings = window_get_user_data(window);
  SettingsKeyboardData *keyboard = (SettingsKeyboardData *)settings->callbacks;
  const unsigned int reads = s_status_reads;
  window_set_on_screen(window, false, true);
  s_top_window = NULL;
  // Another app or submenu owns user data while this window is covered. Reloading
  // through the Settings helper here would dereference this unrelated pointer.
  app_state_set_user_data((void *)1);
  s_status = (BTKeyboardStatus){.state = BTKeyboardStateConnected, .bonded = true};
  prv_send_status_event();
  cl_assert_equal_i(s_status_reads, reads);
  cl_assert_equal_i(keyboard->status.state, BTKeyboardStateIdle);
  app_state_set_user_data(settings);
  s_top_window = window;
  window_set_on_screen(window, true, true);
  cl_assert_equal_i(keyboard->status.state, BTKeyboardStateConnected);
  cl_assert_equal_i(s_status_reads, reads + 1);
  prv_close_keyboard(window);
}

void test_settings_keyboard__reconnect_is_connecting_and_select_cancels(void) {
  s_status = (BTKeyboardStatus){.state = BTKeyboardStateEncrypting, .bonded = true};
  Window *window = prv_open_keyboard();
  SettingsData *settings = window_get_user_data(window);
  SettingsKeyboardData *keyboard = (SettingsKeyboardData *)settings->callbacks;
  cl_assert_equal_s(prv_status_text(&keyboard->status), "Connecting...");
  cl_assert_equal_s(prv_action_text(&keyboard->status), "Cancel");
  keyboard->callbacks.select_click(&keyboard->callbacks, KeyboardRowAction);
  cl_assert_equal_i(s_disconnect_requests, 1);
  cl_assert_equal_i(s_pair_requests, 0);
  cl_assert_equal_i(s_connect_requests, 0);
  prv_close_keyboard(window);
}

void test_settings_keyboard__pair_connect_disconnect_and_forget_actions(void) {
  Window *window = prv_open_keyboard();
  SettingsData *settings = window_get_user_data(window);
  SettingsKeyboardData *keyboard = (SettingsKeyboardData *)settings->callbacks;
  keyboard->callbacks.select_click(&keyboard->callbacks, KeyboardRowAction);
  cl_assert_equal_i(s_pair_requests, 1);
  s_status.bonded = true;
  prv_send_status_event();
  keyboard->callbacks.select_click(&keyboard->callbacks, KeyboardRowAction);
  cl_assert_equal_i(s_connect_requests, 1);
  s_status.state = BTKeyboardStateConnected;
  prv_send_status_event();
  keyboard->callbacks.select_click(&keyboard->callbacks, KeyboardRowAction);
  cl_assert_equal_i(s_disconnect_requests, 1);
  keyboard->callbacks.select_click(&keyboard->callbacks, KeyboardRowForget);
  cl_assert_equal_i(s_forget_requests, 1);
  s_status.state = BTKeyboardStateDisabled;
  prv_send_status_event();
  keyboard->callbacks.select_click(&keyboard->callbacks, KeyboardRowAction);
  cl_assert_equal_i(s_pair_requests + s_connect_requests + s_disconnect_requests, 3);
  prv_close_keyboard(window);
}

void test_settings_keyboard__instructions_fit_at_every_content_size(void) {
  Window *window = prv_open_keyboard();
  SettingsData *settings = window_get_user_data(window);
  SettingsKeyboardData *keyboard = (SettingsKeyboardData *)settings->callbacks;
  const BTKeyboardStatus states[] = {
    {.state = BTKeyboardStateIdle},
    {.state = BTKeyboardStatePairing, .passkey_valid = true, .passkey = 12345},
    {.state = BTKeyboardStateDisabled},
  };
  for (int size = 0; size < NumPreferredContentSizes; ++size) {
    system_theme_set_content_size(size);
    for (unsigned int state = 0; state < ARRAY_LENGTH(states); ++state) {
      keyboard->status = states[state];
      const int16_t height =
          keyboard->callbacks.row_height(&keyboard->callbacks, KeyboardRowAction, true);
      const int16_t title_height =
          fonts_get_font_height(system_theme_get_font(TextStyleFont_MenuCellTitle));
      const GFont footer = system_theme_get_font(TextStyleFont_Footer);
      const GRect unconstrained = GRect(0, 0, DISP_COLS - PBL_IF_RECT_ELSE(10, 40), 1000);
      const GSize text_size = app_graphics_text_layout_get_content_size(
          prv_instructions(&keyboard->status), footer, unconstrained, GTextOverflowModeWordWrap,
          GTextAlignmentCenter);
      // The entire selected cell fits in the menu viewport, and its body fits
      // without dropping any wrapped line. Round cells need not all be 84 px.
      cl_assert(height <= settings->menu_layer.scroll_layer.layer.bounds.size.h);
      cl_assert(text_size.h <= height - title_height - 10);
    }
  }
  prv_close_keyboard(window);
}

void test_settings_keyboard__render_states(void) {
  const BTKeyboardStatus states[] = {
    {.state = BTKeyboardStateIdle},
    {.state = BTKeyboardStatePairing, .passkey_valid = true, .passkey = 12345},
    {.state = BTKeyboardStateConnected, .bonded = true, .name = "Test keyboard"},
    {.state = BTKeyboardStateConnected, .bonded = true, .name = "Test keyboard"},
    {.state = BTKeyboardStateError, .error = 5},
    {.state = BTKeyboardStateError, .bonded = true, .name = "Test keyboard", .error = 5},
    {.state = BTKeyboardStateDisabled},
  };
  const uint16_t selections[] = {1, 1, 1, 2, 0, 0, 1};
  const int padding = 8;
  GBitmap *grid =
      gbitmap_create_blank(GSize(padding + NumPreferredContentSizes * (DISP_COLS + padding),
                                 padding + ARRAY_LENGTH(states) * (DISP_ROWS + padding)),
                           GBitmapFormat8Bit);
  cl_assert(grid);
  memset(grid->addr, GColorDarkGrayARGB8, grid->row_size_bytes * grid->bounds.size.h);
  for (int size = 0; size < NumPreferredContentSizes; ++size) {
    system_theme_set_content_size(size);
    for (unsigned int state = 0; state < ARRAY_LENGTH(states); ++state) {
      s_status = states[state];
      test_graphics_context_reset(&s_ctx, s_framebuffer);
      graphics_context_set_antialiased(&s_ctx, false);
      Window *window = prv_open_keyboard();
      SettingsData *settings = window_get_user_data(window);
      menu_layer_set_selected_index(&settings->menu_layer, MenuIndex(0, selections[state]),
                                    MenuRowAlignCenter, false);
      window_render(window, &s_ctx);
      window_render(window, &s_ctx);
      GBitmap *frame = graphics_capture_frame_buffer(&s_ctx);
      cl_assert(frame);
      const int left = padding + size * (DISP_COLS + padding);
      const int top = padding + state * (DISP_ROWS + padding);
      for (int y = 0; y < DISP_ROWS; ++y) {
        const GBitmapDataRowInfo row = gbitmap_get_data_row_info(frame, y);
        uint8_t *dest = (uint8_t *)grid->addr + (top + y) * grid->row_size_bytes + left;
        memset(dest, GColorBlackARGB8, DISP_COLS);
        memcpy(dest + row.min_x, row.data + row.min_x, row.max_x - row.min_x + 1);
      }
      if (getenv("PEBBLE_KEYBOARD_CAPTURE_FRAMES")) {
        GBitmap *single = gbitmap_create_blank(GSize(DISP_COLS, DISP_ROWS), GBitmapFormat8Bit);
        cl_assert(single);
        for (int y = 0; y < DISP_ROWS; ++y) {
          const uint8_t *source = (uint8_t *)grid->addr + (top + y) * grid->row_size_bytes + left;
          memcpy((uint8_t *)single->addr + y * single->row_size_bytes, source, DISP_COLS);
        }
        char name[96];
        snprintf(name, sizeof(name), "keyboard-%s-size%d-state%d.pbi", PLATFORM_NAME, size, state);
        cl_assert(tests_write_gbitmap_to_pbi(single, name));
        gbitmap_destroy(single);
      }
      graphics_release_frame_buffer(&s_ctx, frame);
      prv_close_keyboard(window);
    }
  }
  char name[96];
  snprintf(name, sizeof(name), "test_settings_keyboard__render_states~%s.pbi", PLATFORM_NAME);
  if (getenv("PEBBLE_KEYBOARD_CAPTURE_FRAMES")) {
    cl_assert(tests_write_gbitmap_to_pbi(grid, name));
  }
  cl_assert(gbitmap_pbi_eq(grid, name));
  gbitmap_destroy(grid);
}
