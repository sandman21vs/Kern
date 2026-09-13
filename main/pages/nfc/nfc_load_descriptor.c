// Load Descriptor from NFC — read a card, unseal if sealed, validate

#include "nfc_load_descriptor.h"
#include "../../core/descriptor_checksum.h"
#include "../../core/kef.h"
#include "../../ui/dialog.h"
#include "../../utils/secure_mem.h"
#include "../shared/descriptor_loader.h"
#include "../shared/kef_decrypt_page.h"
#include "nfc_tap_page.h"

#include <lvgl.h>
#include <nfc.h>
#include <stdlib.h>
#include <string.h>

static void (*return_callback)(void) = NULL;
static void (*success_callback)(void) = NULL;

/* Owned across the validator call, which outlives the callback that built it */
static char *pending_descriptor = NULL;

/* ---------- Navigation ---------- */

static void go_back(void) {
  if (return_callback)
    return_callback();
}

static void drop_pending(void) {
  free(pending_descriptor);
  pending_descriptor = NULL;
}

/* ---------- Validation tail ---------- */

static void descriptor_validation_cb(descriptor_validation_result_t result,
                                     void *user_data) {
  (void)user_data;
  drop_pending();

  if (result == VALIDATION_SUCCESS) {
    if (success_callback)
      success_callback();
    return;
  }

  descriptor_loader_show_error(result);
  go_back();
}

/*
 * Turn a card payload into a C string, or refuse it.
 *
 * descriptor_loader_process_string() takes a const char *, so an embedded NUL
 * in a payload a stranger chose would silently truncate the descriptor to a
 * shorter one that still parses — a different wallet, loaded without a word.
 * Every byte a descriptor may legitimately contain is printable ASCII, so
 * requiring that closes the gap without excluding anything real.
 */
static char *payload_to_string(const uint8_t *data, size_t len) {
  if (!data || len == 0)
    return NULL;

  for (size_t i = 0; i < len; i++)
    if (data[i] < 0x20 || data[i] > 0x7E)
      return NULL;

  char *str = malloc(len + 1);
  if (!str)
    return NULL;
  memcpy(str, data, len);
  str[len] = '\0';
  return str;
}

/* ---------- Sealed path ---------- */

static void return_from_kef_decrypt(void) {
  kef_decrypt_page_destroy();
  go_back();
}

static void success_from_kef_decrypt(const uint8_t *data, size_t len) {
  /* Copy before destroying the page — data is page-owned. */
  char *descriptor_str = payload_to_string(data, len);
  kef_decrypt_page_destroy();

  if (!descriptor_str) {
    /* The envelope opened and what came out is not a descriptor. On a KEF
       version with a 16-bit hidden auth a wrong password gets through about
       once in 65536 tries, so this is a password message as much as a content
       one. No checksum demand here: the envelope already authenticated it. */
    dialog_show_error_timeout("Card does not hold a descriptor", go_back, 0);
    return;
  }

  drop_pending();
  pending_descriptor = descriptor_str;
  descriptor_loader_process_string(pending_descriptor, descriptor_validation_cb,
                                   NULL);
}

/* ---------- Card read ---------- */

static void on_tag(const nfc_tag_t *tag) {
  uint8_t *data = NULL;
  size_t len = 0;
  esp_err_t ret =
      nfc_read_record(tag, NFC_ACCEPT(NFC_RECORD_TYPE_DESCRIPTOR), &data, &len);

  /* The antenna goes down before any password prompt: a card page that stays
     live behind a keyboard is exactly what the tap page exists to prevent. */
  nfc_tap_page_destroy();

  if (ret != ESP_OK) {
    /* A blank card, an unreadable one, and a card holding a seed all say the
       same thing. The wrong-type case is refused at the header, so nothing was
       allocated and no password was asked for. */
    dialog_show_error_timeout("No descriptor on this card", go_back, 0);
    return;
  }

  /* A payload of printable ASCII can never be mistaken for a KEF envelope:
     kef_is_envelope reads a version byte that is always below 32, where a
     descriptor always has a character of 0x20 or above. The fork is
     unambiguous by construction, not by luck. */
  if (kef_is_envelope(data, len)) {
    kef_decrypt_page_create(lv_screen_active(), return_from_kef_decrypt,
                            success_from_kef_decrypt, data, len);
    kef_decrypt_page_show();
    SECURE_FREE_BUFFER(data, len); /* the page copied it */
    return;
  }

  char *descriptor_str = payload_to_string(data, len);
  SECURE_FREE_BUFFER(data, len);

  if (!descriptor_str) {
    /* The header already said descriptor, so this is not an empty card: it is
       ours and its content is broken — a byte flipped outside printable ASCII,
       or a write cut short that left zeros at the tail. Saying "no descriptor"
       would send the user looking for a different card instead of rewriting
       this one. */
    dialog_show_error_timeout("Card data is damaged", go_back, 0);
    return;
  }

  /*
   * An unsealed record has no integrity of its own — the card format carries
   * no checksum, because a sealed record is authenticated by its envelope. So
   * the descriptor's own BIP-380 checksum is the substitute, and it has to be
   * verified here or nowhere: libwally parses a descriptor whether or not the
   * checksum agrees, and descriptor_to_unambiguous() strips it before the
   * validator ever sees it.
   *
   * It matters more than it sounds. Only the xpubs defend themselves, being
   * base58check. A flipped bit in a fingerprint, a derivation index, a
   * threshold or a script wrapper is accepted in silence, and the wallet
   * watches different addresses than the one that wrote the card.
   */
  if (!descriptor_checksum_verify_text(descriptor_str)) {
    free(descriptor_str);
    /* Deliberately not "no descriptor": the record parsed and its content did
       not, which is the difference between tapping again and rewriting the
       card. */
    dialog_show_error_timeout("Card data is damaged", go_back, 0);
    return;
  }

  drop_pending();
  pending_descriptor = descriptor_str;
  descriptor_loader_process_string(pending_descriptor, descriptor_validation_cb,
                                   NULL);
}

static void tap_cancel_cb(void) {
  nfc_tap_page_destroy();
  go_back();
}

/* ---------- Page lifecycle ---------- */

void nfc_load_descriptor_page_create(lv_obj_t *parent, void (*return_cb)(void),
                                     void (*success_cb)(void)) {
  if (!parent)
    return;

  /* See the note in login_settings.c: a session expiry cleans the screen
     without running page teardown. pending_descriptor is plain heap that
     survived the clean, so it is freed rather than leaked. */
  drop_pending();
  return_callback = return_cb;
  success_callback = success_cb;

  nfc_tap_page_create(parent, "Load from NFC", "Reads a wallet descriptor",
                      tap_cancel_cb, on_tag);
}

void nfc_load_descriptor_page_show(void) { nfc_tap_page_show(); }

void nfc_load_descriptor_page_hide(void) { nfc_tap_page_hide(); }

void nfc_load_descriptor_page_destroy(void) {
  nfc_tap_page_destroy();
  kef_decrypt_page_destroy();
  drop_pending();
  return_callback = NULL;
  success_callback = NULL;
}
