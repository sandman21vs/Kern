// NFC Settings Page — enable card storage and probe the reader

#include "nfc_settings.h"
#include "../../core/settings.h"
#include "../../ui/dialog.h"
#include "../../ui/input_helpers.h"
#include "../../ui/settings_row.h"
#include "../../ui/theme.h"
#include "../../ui/theme_widgets.h"

#include <bsp/esp-bsp.h>
#include <lvgl.h>
#include <nfc.h>

static const char *NFC_HELP =
    "Store encrypted seed backups on NFC cards through an external WS1850S "
    "reader.\n\nThe reader is a radio. While this is off the firmware never "
    "touches it, even with the module plugged in -- no bus access, no power "
    "to the antenna. While it is on, the antenna still only comes up inside "
    "a card page, and only the encrypted backup crosses it, never the seed "
    "itself.";

static lv_obj_t *nfc_settings_screen = NULL;
static lv_obj_t *back_button = NULL;
static lv_obj_t *test_row = NULL;
static void (*return_callback)(void) = NULL;

/* The reader is off-limits unless the user turned the feature on. Kept as a
 * function so every path that could reach the hardware asks the same
 * question. */
static void set_test_enabled(bool enabled) {
  if (!test_row)
    return;
  if (enabled) {
    lv_obj_clear_state(test_row, LV_STATE_DISABLED);
    lv_obj_add_flag(test_row, LV_OBJ_FLAG_CLICKABLE);
  } else {
    lv_obj_add_state(test_row, LV_STATE_DISABLED);
    lv_obj_remove_flag(test_row, LV_OBJ_FLAG_CLICKABLE);
  }
}

static void toggle_cb(lv_event_t *e) {
  bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  settings_set_nfc_enabled(enabled);
  set_test_enabled(enabled);
}

static void test_reader_cb(lv_event_t *e) {
  (void)e;

  /* Backstop for the greyed-out row above: nothing may reach the bus while
     the feature is off, whatever the widget state happens to be. */
  if (!settings_get_nfc_enabled())
    return;

  esp_err_t ret = nfc_init(bsp_i2c_get_handle());
  if (ret == ESP_OK) {
    /* Leave nothing attached: the probe is a one-off, not a session. */
    nfc_deinit();
    dialog_show_info("NFC", "Reader detected.", NULL, NULL,
                     DIALOG_STYLE_OVERLAY);
  } else {
    dialog_show_error_timeout("No reader on the I2C bus", NULL, 0);
  }
}

static void back_cb(lv_event_t *e) {
  (void)e;
  if (return_callback)
    return_callback();
}

void nfc_settings_page_create(lv_obj_t *parent, void (*return_cb)(void)) {
  if (!parent)
    return;

  /* Statics may dangle if session expiry cleaned the screen while this page
     was open; drop them before rebuilding. */
  nfc_settings_screen = NULL;
  back_button = NULL;
  test_row = NULL;
  return_callback = return_cb;

  nfc_settings_screen = lv_obj_create(parent);
  lv_obj_set_size(nfc_settings_screen, LV_PCT(100), LV_PCT(100));
  theme_apply_screen(nfc_settings_screen);
  lv_obj_clear_flag(nfc_settings_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(nfc_settings_screen, theme_default_padding(), 0);
  lv_obj_set_style_pad_top(nfc_settings_screen, theme_small_padding(), 0);
  lv_obj_set_style_pad_bottom(nfc_settings_screen, theme_small_padding(), 0);
  lv_obj_set_flex_flow(nfc_settings_screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(nfc_settings_screen, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(nfc_settings_screen, theme_default_padding(), 0);

  /* Title sits in a band as tall as the corner buttons so it lines up beside
     the back button instead of underneath it. */
  lv_obj_t *nav_bar = lv_obj_create(nfc_settings_screen);
  lv_obj_set_size(nav_bar, LV_PCT(100), theme_corner_button_height());
  theme_apply_transparent_container(nav_bar);
  lv_obj_set_flex_flow(nav_bar, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(nav_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *title = theme_create_label(nav_bar, "NFC", true);
  lv_obj_set_style_text_font(title, theme_font_small(), 0);

  lv_obj_t *content = lv_obj_create(nfc_settings_screen);
  lv_obj_set_width(content, LV_PCT(100));
  lv_obj_set_flex_grow(content, 1);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_pad_all(content, 0, 0);
  lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(content, LV_DIR_VER);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(content, 4, 0);

  bool enabled = settings_get_nfc_enabled();

  settings_row_toggle(content, "NFC card storage", enabled, toggle_cb,
                      "NFC card storage", NFC_HELP);

  test_row = settings_row_action(content, "Test reader", test_reader_cb);
  set_test_enabled(enabled);

  lv_obj_t *hint = theme_create_label(
      content, "The reader is only powered while NFC is on.", true);
  lv_obj_set_width(hint, LV_PCT(100));
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

  back_button = ui_create_back_button(parent, back_cb);
}

void nfc_settings_page_show(void) {
  if (nfc_settings_screen)
    lv_obj_clear_flag(nfc_settings_screen, LV_OBJ_FLAG_HIDDEN);
  if (back_button)
    lv_obj_clear_flag(back_button, LV_OBJ_FLAG_HIDDEN);
}

void nfc_settings_page_hide(void) {
  if (nfc_settings_screen)
    lv_obj_add_flag(nfc_settings_screen, LV_OBJ_FLAG_HIDDEN);
  if (back_button)
    lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
}

void nfc_settings_page_destroy(void) {
  if (back_button) {
    lv_obj_del(back_button);
    back_button = NULL;
  }
  if (nfc_settings_screen) {
    lv_obj_del(nfc_settings_screen);
    nfc_settings_screen = NULL;
  }
  test_row = NULL;
  return_callback = NULL;
}
