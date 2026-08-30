// Load Mnemonic from NFC — read a card, decrypt, confirm the key

#include "nfc_load_mnemonic.h"
#include "../../core/kef.h"
#include "../../qr/encoder.h"
#include "../../ui/dialog.h"
#include "../../utils/secure_mem.h"
#include "../shared/kef_decrypt_page.h"
#include "../shared/key_confirmation.h"
#include "nfc_tap_page.h"

#include <lvgl.h>
#include <nfc.h>
#include <stdlib.h>
#include <string.h>

static void (*return_callback)(void) = NULL;
static void (*success_callback)(void) = NULL;

static void go_back(void) {
  if (return_callback)
    return_callback();
}

/* ---------- Key confirmation ---------- */

static void return_from_key_confirmation(void) {
  key_confirmation_page_destroy();
  go_back();
}

static void success_from_key_confirmation(void) {
  key_confirmation_page_destroy();
  if (success_callback)
    success_callback();
}

/* ---------- Decrypt ---------- */

static void return_from_kef_decrypt(void) {
  kef_decrypt_page_destroy();
  go_back();
}

static void success_from_kef_decrypt(const uint8_t *data, size_t len) {
  /*
   * Decrypting does not make these bytes ours. KEF versions with a 16-bit
   * hidden auth let a wrong password through about once in 65536 tries, and a
   * planted card could have been encrypted with a password its author chose.
   * So the payload passes one narrow gate: compact SeedQR only, 16 or 32
   * bytes, BIP39 checksum verified. mnemonic_qr_to_mnemonic() would also take
   * plaintext words and numeric SeedQR — formats Kern never writes to a card,
   * and therefore formats no genuine card can present.
   */
  char *mnemonic = mnemonic_qr_compact_to_mnemonic(data, len);
  if (!mnemonic) {
    kef_decrypt_page_destroy();
    dialog_show_error_timeout("Card does not hold a seed", go_back, 0);
    return;
  }

  key_confirmation_page_create(lv_screen_active(), return_from_key_confirmation,
                               success_from_key_confirmation, mnemonic,
                               strlen(mnemonic));
  key_confirmation_page_show();
  SECURE_FREE_STRING(mnemonic);
  kef_decrypt_page_destroy();
}

/* ---------- Card read ---------- */

static void on_tag(const nfc_tag_t *tag) {
  uint8_t *envelope = NULL;
  size_t envelope_len = 0;

  esp_err_t ret = nfc_read_record(tag, &envelope, &envelope_len);
  nfc_tap_page_destroy();

  if (ret != ESP_OK) {
    dialog_show_error_timeout("No Kern backup on this card", go_back, 0);
    return;
  }

  if (!kef_is_envelope(envelope, envelope_len)) {
    SECURE_FREE_BUFFER(envelope, envelope_len);
    dialog_show_error_timeout("Invalid encrypted data", go_back, 0);
    return;
  }

  kef_decrypt_page_create(lv_screen_active(), return_from_kef_decrypt,
                          success_from_kef_decrypt, envelope, envelope_len);
  kef_decrypt_page_show();
  SECURE_FREE_BUFFER(envelope, envelope_len); /* kef_decrypt_page copies it */
}

static void tap_cancel_cb(void) {
  nfc_tap_page_destroy();
  go_back();
}

/* ---------- Page lifecycle ---------- */

void nfc_load_mnemonic_page_create(lv_obj_t *parent, void (*return_cb)(void),
                                   void (*success_cb)(void)) {
  if (!parent)
    return;

  return_callback = return_cb;
  success_callback = success_cb;

  nfc_tap_page_create(parent, "Load from NFC", "Reads an encrypted backup",
                      tap_cancel_cb, on_tag);
  nfc_tap_page_show();
}

void nfc_load_mnemonic_page_show(void) { nfc_tap_page_show(); }

void nfc_load_mnemonic_page_hide(void) { nfc_tap_page_hide(); }

void nfc_load_mnemonic_page_destroy(void) {
  nfc_tap_page_destroy();
  return_callback = NULL;
  success_callback = NULL;
}
