// Store Mnemonic on NFC — KEF encrypt, then write the envelope to a card

#include "nfc_store_mnemonic.h"
#include "../../core/key.h"
#include "../../qr/encoder.h"
#include "../../ui/dialog.h"
#include "../../ui/theme_widgets.h"
#include "../../utils/secure_mem.h"
#include "../shared/kef_encrypt_page.h"
#include "nfc_tap_page.h"

#include <lvgl.h>
#include <nfc.h>
#include <stdio.h>
#include <string.h>
#include <wally_core.h>

static lv_obj_t *main_screen = NULL;
static void (*return_callback)(void) = NULL;

/* Data to encrypt (compact SeedQR binary entropy) */
static unsigned char *compact_seedqr_data = NULL;
static size_t compact_seedqr_len = 0;

/* Pending write — valid between encrypt success and kef_encrypt_page_destroy */
static const uint8_t *pending_envelope = NULL;
static size_t pending_envelope_len = 0;

/* ---------- Navigation ---------- */

static void go_back(void) {
  if (return_callback)
    return_callback();
}

static void finish_dialog_cb(void *user_data) {
  (void)user_data;
  go_back();
}

static void teardown(void) {
  nfc_tap_page_destroy();
  kef_encrypt_page_destroy();
  pending_envelope = NULL;
  pending_envelope_len = 0;
}

/* ---------- Card write ---------- */

static void do_write(void) {
  /* Re-select rather than trusting the tag from the poll: a dialog may have
     been up in between, and the card only had to drift a centimetre. */
  nfc_tag_t tag;
  if (nfc_poll(&tag) != ESP_OK) {
    teardown();
    dialog_show_error_timeout("Card removed", go_back, 0);
    return;
  }

  esp_err_t ret =
      nfc_write_record(&tag, pending_envelope, pending_envelope_len);
  teardown();

  if (ret == ESP_OK) {
    dialog_show_info("Saved", "Mnemonic written to the NFC card.",
                     finish_dialog_cb, NULL, DIALOG_STYLE_OVERLAY);
  } else if (ret == ESP_ERR_INVALID_SIZE) {
    dialog_show_error_timeout("Card too small", go_back, 0);
  } else {
    dialog_show_error_timeout("Failed to write card", go_back, 0);
  }
}

static void overwrite_confirm_cb(bool confirmed, void *user_data) {
  (void)user_data;
  if (confirmed) {
    do_write();
    return;
  }
  teardown();
  go_back();
}

static void on_tag(const nfc_tag_t *tag) {
  if (nfc_has_record(tag)) {
    dialog_show_danger_confirm("This card already holds a backup. Overwrite?",
                               overwrite_confirm_cb, NULL,
                               DIALOG_STYLE_OVERLAY);
    return;
  }
  do_write();
}

static void tap_cancel_cb(void) {
  teardown();
  go_back();
}

/* ---------- Encrypt callbacks ---------- */

static void encrypt_return_cb(void) {
  kef_encrypt_page_destroy();
  go_back();
}

static void encrypt_success_cb(const char *id, const uint8_t *envelope,
                               size_t len) {
  (void)id; /* the card holds one record, so the ID stays inside the envelope */

  /* Envelope stays valid until kef_encrypt_page_destroy() */
  pending_envelope = envelope;
  pending_envelope_len = len;

  kef_encrypt_page_hide();
  nfc_tap_page_create(lv_screen_active(), "Save to NFC",
                      "The encrypted backup will be written", tap_cancel_cb,
                      on_tag);
  nfc_tap_page_show();
}

/* ---------- Page lifecycle ---------- */

void nfc_store_mnemonic_page_create(lv_obj_t *parent, void (*return_cb)(void)) {
  if (!parent || !key_is_loaded())
    return;

  /* See the note in login_settings.c: a session expiry cleans the screen
     without running page teardown, so stale pointers go first. */
  main_screen = NULL;
  pending_envelope = NULL;
  pending_envelope_len = 0;
  return_callback = return_cb;

  char *mnemonic = NULL;
  if (!key_get_mnemonic(&mnemonic) || !mnemonic) {
    dialog_show_error_timeout("Failed to get mnemonic", return_cb, 0);
    return;
  }

  compact_seedqr_data =
      mnemonic_to_compact_seedqr(mnemonic, &compact_seedqr_len);

  secure_memzero(mnemonic, strlen(mnemonic));
  wally_free_string(mnemonic);

  if (!compact_seedqr_data) {
    dialog_show_error_timeout("Failed to prepare data", return_cb, 0);
    return;
  }

  main_screen = theme_create_page_container(parent);
  theme_create_page_title(main_screen, "Save to NFC");

  kef_encrypt_page_create(parent, encrypt_return_cb, encrypt_success_cb,
                          compact_seedqr_data, compact_seedqr_len, NULL);
}

void nfc_store_mnemonic_page_show(void) {
  if (main_screen)
    lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_HIDDEN);
}

void nfc_store_mnemonic_page_hide(void) {
  if (main_screen)
    lv_obj_add_flag(main_screen, LV_OBJ_FLAG_HIDDEN);
}

void nfc_store_mnemonic_page_destroy(void) {
  teardown();

  SECURE_FREE_BUFFER(compact_seedqr_data, compact_seedqr_len);
  compact_seedqr_len = 0;

  if (main_screen) {
    lv_obj_del(main_screen);
    main_screen = NULL;
  }

  return_callback = NULL;
}
