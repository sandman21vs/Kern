// NFC Tap Page — card presence prompt and RF field lifecycle

#include "nfc_tap_page.h"
#include "../../core/settings.h"
#include "../../ui/dialog.h"
#include "../../ui/input_helpers.h"
#include "../../ui/theme.h"
#include "../../ui/theme_widgets.h"

#include <bsp/esp-bsp.h>
#include <lvgl.h>

#define POLL_INTERVAL_MS 200

static lv_obj_t *tap_screen = NULL;
static lv_obj_t *back_button = NULL;
static lv_timer_t *poll_timer = NULL;
static void (*cancel_callback)(void) = NULL;
static nfc_tap_cb_t tag_callback = NULL;

static void stop_polling(void) {
  if (poll_timer) {
    lv_timer_del(poll_timer);
    poll_timer = NULL;
  }
}

static void poll_timer_cb(lv_timer_t *timer) {
  (void)timer;

  nfc_tag_t tag;
  if (nfc_poll(&tag) != ESP_OK)
    return;

  /* Hand the tag over while it is still selected. Polling stops first so a
     second timer tick cannot re-enter the callback mid-transaction. */
  stop_polling();
  if (tag_callback)
    tag_callback(&tag);
}

static void cancel_cb(lv_event_t *e) {
  (void)e;
  void (*callback)(void) = cancel_callback;
  if (callback)
    callback();
}

void nfc_tap_page_create(lv_obj_t *parent, const char *title, const char *hint,
                         void (*cancel_callback_fn)(void),
                         nfc_tap_cb_t on_tag) {
  if (!parent)
    return;

  /* Statics may dangle if session expiry cleaned the screen under us; drop
     them so destroy does not delete freed objects. */
  stop_polling();
  tap_screen = NULL;
  back_button = NULL;

  cancel_callback = cancel_callback_fn;
  tag_callback = on_tag;

  /* The menu entries that lead here are already gated on the setting, but the
     check is repeated at the hardware boundary so no future caller can reach
     the bus around it. A disabled reader is untouched, not merely unused. */
  if (!settings_get_nfc_enabled()) {
    dialog_show_error_timeout("NFC is disabled", cancel_callback_fn, 0);
    return;
  }

  /* Attach the reader now rather than at boot: with NFC switched off, this
     never runs and the I2C device is never even registered. */
  esp_err_t ret = nfc_init(bsp_i2c_get_handle());
  if (ret != ESP_OK) {
    dialog_show_error_timeout("NFC reader not found", cancel_callback_fn, 0);
    return;
  }

  tap_screen = theme_create_page_container(parent);
  theme_create_page_title(tap_screen, title);

  lv_obj_t *spinner = lv_spinner_create(tap_screen);
  lv_spinner_set_anim_params(spinner, 1000, 60);
  lv_obj_set_size(spinner, theme_min_touch_size() * 2,
                  theme_min_touch_size() * 2);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -theme_button_spacing());
  lv_obj_set_style_arc_color(spinner, highlight_color(), LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, disabled_color(), LV_PART_MAIN);

  lv_obj_t *prompt =
      theme_create_label(tap_screen, "Hold a card to the reader", false);
  lv_obj_align(prompt, LV_ALIGN_CENTER, 0, theme_button_spacing() * 2);

  if (hint) {
    lv_obj_t *hint_label = theme_create_label(tap_screen, hint, true);
    lv_obj_align(hint_label, LV_ALIGN_CENTER, 0, theme_button_spacing() * 4);
  }

  back_button = ui_create_back_button(parent, cancel_cb);

  if (nfc_field_set(true) != ESP_OK) {
    dialog_show_error_timeout("NFC reader failed", cancel_callback_fn, 0);
    return;
  }

  poll_timer = lv_timer_create(poll_timer_cb, POLL_INTERVAL_MS, NULL);
}

void nfc_tap_page_show(void) {
  if (tap_screen)
    lv_obj_clear_flag(tap_screen, LV_OBJ_FLAG_HIDDEN);
  if (back_button)
    lv_obj_clear_flag(back_button, LV_OBJ_FLAG_HIDDEN);
}

void nfc_tap_page_hide(void) {
  if (tap_screen)
    lv_obj_add_flag(tap_screen, LV_OBJ_FLAG_HIDDEN);
  if (back_button)
    lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
}

void nfc_tap_page_destroy(void) {
  stop_polling();

  /* Field down and reader detached before the UI goes, so the antenna is
     never left live behind a dead page. */
  nfc_deinit();

  if (back_button) {
    lv_obj_del(back_button);
    back_button = NULL;
  }
  if (tap_screen) {
    lv_obj_del(tap_screen);
    tap_screen = NULL;
  }

  cancel_callback = NULL;
  tag_callback = NULL;
}
