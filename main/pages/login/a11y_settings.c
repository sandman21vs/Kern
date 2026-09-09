// Accessibility Settings Page — turn the screen reader on and try the speaker
//
// This page lives under the login settings rather than the wallet ones for a
// practical reason: someone who cannot see the screen has to be able to turn
// the reader on before they can be asked for a PIN.

#include "a11y_settings.h"
#include "../../a11y/a11y.h"
#include "../../a11y/speech.h"
#include "../../core/settings.h"
#include "../../ui/dialog.h"
#include "../../ui/input_helpers.h"
#include "../../ui/settings_row.h"
#include "../../ui/theme.h"
#include "../../ui/theme_widgets.h"

#include <lvgl.h>

static const char *SECRETS_HELP =
    "Read seed words, PINs and passphrases aloud.\n\nThe board has a speaker "
    "and no headphone jack, so anything it says can be overheard or recorded "
    "by anyone near it. Leave this off unless you are alone.\n\nIt exists "
    "because with it off there is no way to enter a PIN or check a seed "
    "backup without seeing the screen: the reader plays a tone where those "
    "are and says nothing. Whether the room is safe is a judgement only you "
    "can make.";

static const char *READER_HELP =
    "Read the interface aloud and navigate it by touch, for someone who "
    "cannot see the screen.\n\nDrag a finger across the screen to hear what "
    "is under it. Swipe right or left to step through the screen in order. "
    "Tap twice to activate what was last announced.\n\nAnything spoken can "
    "be overheard. Seed words, PINs and passphrases are never read aloud; "
    "where they are on screen the reader says nothing and plays a short tone "
    "instead.";

static lv_obj_t *a11y_settings_screen = NULL;
static lv_obj_t *back_button = NULL;
static lv_obj_t *test_row = NULL;
static lv_obj_t *secrets_row = NULL;
static void (*return_callback)(void) = NULL;

static void set_row_enabled(lv_obj_t *row, bool enabled) {
  if (!row)
    return;
  if (enabled) {
    lv_obj_clear_state(row, LV_STATE_DISABLED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  } else {
    lv_obj_add_state(row, LV_STATE_DISABLED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
  }
}

/* Both rows below the main switch only mean anything while the reader is on. */
static void set_test_enabled(bool enabled) {
  set_row_enabled(test_row, enabled);
  set_row_enabled(secrets_row, enabled);
}

/* Reached only after the danger dialog is answered. Yes turns it on; anything
   else puts the switch back, because the dialog is the consent and a dismissed
   dialog is not one. */
static void secrets_confirmed(bool yes, void *user_data) {
  lv_obj_t *sw = (lv_obj_t *)user_data;
  if (yes) {
    settings_set_a11y_secrets(true);
    a11y_set_speak_secrets(true);
    a11y_announce("Sensitive data will be spoken");
    return;
  }
  if (sw)
    lv_obj_clear_state(sw, LV_STATE_CHECKED);
}

static void secrets_toggle_cb(lv_event_t *e) {
  lv_obj_t *sw = lv_event_get_target(e);
  const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

  if (!on) {
    settings_set_a11y_secrets(false);
    a11y_set_speak_secrets(false);
    return;
  }

  /* Turning it on is the destructive direction here, so it gets the danger
     dialog: red border, warning icon, and the safe answer in green. */
  dialog_show_danger_confirm(
      "Read seed words, PINs and passphrases out loud?\n\nAnyone near the "
      "device will hear them.",
      secrets_confirmed, sw, DIALOG_STYLE_OVERLAY);
}

static void toggle_cb(lv_event_t *e) {
  const bool enabled =
      lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);

  if (enabled && !a11y_init()) {
    /* No codec answered, or no speaker circuit on this board. Put the switch
       back rather than leave it claiming a reader that cannot speak. */
    lv_obj_clear_state(lv_event_get_target(e), LV_STATE_CHECKED);
    dialog_show_error_timeout("No speaker on this board", NULL, 0);
    return;
  }

  settings_set_a11y_enabled(enabled);
  a11y_set_enabled(enabled);
  set_test_enabled(enabled);

  if (enabled)
    a11y_announce("Screen reader on");
}

static void test_speaker_cb(lv_event_t *e) {
  (void)e;

  /* Backstop for the greyed-out row: nothing reaches the amplifier while the
     reader is off, whatever state the widget happens to be in. */
  if (!settings_get_a11y_enabled())
    return;

  speech_say("Screen reader");
}

static void back_cb(lv_event_t *e) {
  (void)e;
  if (return_callback)
    return_callback();
}

void a11y_settings_page_create(lv_obj_t *parent, void (*return_cb)(void)) {
  if (!parent)
    return;

  /* Statics may dangle if session expiry cleaned the screen while this page
     was open; drop them before rebuilding. */
  a11y_settings_screen = NULL;
  back_button = NULL;
  test_row = NULL;
  secrets_row = NULL;
  return_callback = return_cb;

  a11y_settings_screen = lv_obj_create(parent);
  lv_obj_set_size(a11y_settings_screen, LV_PCT(100), LV_PCT(100));
  theme_apply_screen(a11y_settings_screen);
  lv_obj_clear_flag(a11y_settings_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(a11y_settings_screen, theme_default_padding(), 0);
  lv_obj_set_style_pad_top(a11y_settings_screen, theme_small_padding(), 0);
  lv_obj_set_style_pad_bottom(a11y_settings_screen, theme_small_padding(), 0);
  lv_obj_set_flex_flow(a11y_settings_screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(a11y_settings_screen, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(a11y_settings_screen, theme_default_padding(), 0);

  lv_obj_t *nav_bar = lv_obj_create(a11y_settings_screen);
  lv_obj_set_size(nav_bar, LV_PCT(100), theme_corner_button_height());
  theme_apply_transparent_container(nav_bar);
  lv_obj_set_flex_flow(nav_bar, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(nav_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *title = theme_create_label(nav_bar, "Accessibility", true);
  lv_obj_set_style_text_font(title, theme_font_small(), 0);

  lv_obj_t *content = lv_obj_create(a11y_settings_screen);
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

  const bool enabled = settings_get_a11y_enabled();

  settings_row_toggle(content, "Screen reader", enabled, toggle_cb,
                      "Screen reader", READER_HELP);

  secrets_row = settings_row_toggle(
      content, "Speak sensitive data", settings_get_a11y_secrets(),
      secrets_toggle_cb, "Speak sensitive data", SECRETS_HELP);

  test_row = settings_row_action(content, "Test speaker", test_speaker_cb);
  set_test_enabled(enabled);

  lv_obj_t *hint =
      theme_create_label(content, "Needs a speaker wired to the board.", true);
  lv_obj_set_width(hint, LV_PCT(100));
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

  back_button = ui_create_back_button(parent, back_cb);
}

void a11y_settings_page_show(void) {
  if (a11y_settings_screen)
    lv_obj_clear_flag(a11y_settings_screen, LV_OBJ_FLAG_HIDDEN);
  if (back_button)
    lv_obj_clear_flag(back_button, LV_OBJ_FLAG_HIDDEN);
}

void a11y_settings_page_hide(void) {
  if (a11y_settings_screen)
    lv_obj_add_flag(a11y_settings_screen, LV_OBJ_FLAG_HIDDEN);
  if (back_button)
    lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
}

void a11y_settings_page_destroy(void) {
  if (back_button) {
    lv_obj_del(back_button);
    back_button = NULL;
  }
  if (a11y_settings_screen) {
    lv_obj_del(a11y_settings_screen);
    a11y_settings_screen = NULL;
  }
  test_row = NULL;
  secrets_row = NULL;
  return_callback = NULL;
}
