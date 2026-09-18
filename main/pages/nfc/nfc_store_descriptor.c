// Store Descriptor on NFC — seal or not, then write the record to a card

#include "nfc_store_descriptor.h"
#include "../../core/descriptor_checksum.h"
#include "../../ui/dialog.h"
#include "../../ui/theme_widgets.h"
#include "../../utils/secure_mem.h"
#include "../../utils/session_cleanup.h"
#include "../shared/kef_encrypt_page.h"
#include "nfc_tap_page.h"

#include <lvgl.h>
#include <nfc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *main_screen = NULL;
static void (*return_callback)(void) = NULL;
static lv_obj_t *progress_dialog = NULL;

/* The canonical descriptor string, checksum included. Owned here. */
static char *descriptor_text = NULL;
static size_t descriptor_text_len = 0;

/* What goes on the card. For the sealed form this points into the KEF encrypt
   page's buffer and is only valid until kef_encrypt_page_destroy(); for the
   plaintext form it points at descriptor_text. Never owned. */
static const uint8_t *pending_payload = NULL;
static size_t pending_payload_len = 0;

/* ---------- Navigation ---------- */

static void go_back(void) {
  if (return_callback)
    return_callback();
}

static void finish_dialog_cb(void *user_data) {
  (void)user_data;
  go_back();
}

static void dismiss_progress(void) {
  if (progress_dialog) {
    lv_obj_del(progress_dialog);
    progress_dialog = NULL;
  }
}

static void teardown(void) {
  dismiss_progress();
  nfc_tap_page_destroy();
  kef_encrypt_page_destroy();
  pending_payload = NULL;
  pending_payload_len = 0;
}

/* Refuse before the antenna comes up, naming the numbers: "too large" without
   them leaves the user with no way to judge how much has to go.

   Never advise sealing. KEF_V20_GCM_E4 does not compress, so the envelope is
   the text plus 29 bytes: a descriptor too big in the clear is too big sealed.
   The one useful hint runs the other way — a sealed descriptor that overflows
   may still fit unencrypted, and the user can choose that knowingly. */
static bool payload_fits(size_t len, bool sealed) {
  if (len <= NFC_MAX_PAYLOAD)
    return true;

  char msg[160];
  if (sealed && descriptor_text_len <= NFC_MAX_PAYLOAD)
    snprintf(msg, sizeof(msg),
             "Sealed, the descriptor is %u bytes; a card holds %u.\n"
             "It fits unencrypted.",
             (unsigned)len, (unsigned)NFC_MAX_PAYLOAD);
  else
    snprintf(msg, sizeof(msg), "Descriptor is %u bytes; a card holds %u.",
             (unsigned)len, (unsigned)NFC_MAX_PAYLOAD);
  dialog_show_error_timeout(msg, go_back, 0);
  return false;
}

/* ---------- Card write ---------- */

/* The write blocks the LVGL task for one block at a time — a full record is
   around 45 of them — so the progress dialog has to be given a frame to paint
   before the write starts. Same shape as store_descriptor.c. */
static void deferred_write_cb(lv_timer_t *timer) {
  (void)timer;

  /* Re-select rather than trusting the tag from the poll: a dialog may have
     been up in between, and the card only had to drift a centimetre. */
  nfc_tag_t tag;
  if (nfc_poll(&tag) != ESP_OK) {
    teardown();
    dialog_show_error_timeout("Card removed", go_back, 0);
    return;
  }

  esp_err_t ret = nfc_write_record(&tag, NFC_RECORD_TYPE_DESCRIPTOR,
                                   pending_payload, pending_payload_len);
  teardown();

  if (ret == ESP_OK) {
    dialog_show_info("Saved", "Descriptor written to the NFC card.",
                     finish_dialog_cb, NULL, DIALOG_STYLE_OVERLAY);
  } else if (ret == ESP_ERR_INVALID_SIZE) {
    dialog_show_error_timeout("Card too small", go_back, 0);
  } else {
    dialog_show_error_timeout("Failed to write card", go_back, 0);
  }
}

static void do_write(void) {
  progress_dialog =
      dialog_show_progress("Save to NFC", "Writing...", DIALOG_STYLE_OVERLAY);
  lv_timer_t *t = lv_timer_create(deferred_write_cb, 50, NULL);
  lv_timer_set_repeat_count(t, 1);
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
    dialog_show_danger_confirm(
        "This card already holds a Kern record. Overwrite?",
        overwrite_confirm_cb, NULL, DIALOG_STYLE_OVERLAY);
    return;
  }
  do_write();
}

static void tap_cancel_cb(void) {
  teardown();
  go_back();
}

static void show_tap_page(const char *hint) {
  nfc_tap_page_create(lv_screen_active(), "Save to NFC", hint, tap_cancel_cb,
                      on_tag);
  nfc_tap_page_show();
}

/* ---------- Sealed path ---------- */

static void encrypt_return_cb(void) {
  kef_encrypt_page_destroy();
  go_back();
}

static void encrypt_success_cb(const char *id, const uint8_t *envelope,
                               size_t len) {
  (void)id; /* the card holds one record, so the ID stays inside the envelope */

  if (!payload_fits(len, true)) {
    kef_encrypt_page_destroy();
    return;
  }

  /* Envelope stays valid until kef_encrypt_page_destroy() */
  pending_payload = envelope;
  pending_payload_len = len;

  kef_encrypt_page_hide();
  show_tap_page("The sealed descriptor will be written");
}

/* ---------- Plaintext path ---------- */

static void plaintext_confirm_cb(bool confirmed, void *user_data) {
  (void)user_data;
  if (!confirmed) {
    go_back();
    return;
  }

  pending_payload = (const uint8_t *)descriptor_text;
  pending_payload_len = descriptor_text_len;
  show_tap_page("The descriptor will be written unencrypted");
}

/* ---------- Page lifecycle ---------- */

void nfc_store_descriptor_page_create(
    lv_obj_t *parent, void (*return_cb)(void), bool encrypted,
    const struct wally_descriptor *descriptor) {
  session_cleanup_register(nfc_store_descriptor_page_destroy);
  if (!parent || !descriptor)
    return;

  /* See the note in login_settings.c: a session expiry cleans the screen
     without running page teardown, so stale pointers go first. The LVGL ones
     are dangling and only get nulled; descriptor_text is plain heap that
     survived the clean, so it gets freed rather than leaked. */
  main_screen = NULL;
  progress_dialog = NULL;
  pending_payload = NULL;
  pending_payload_len = 0;
  free(descriptor_text);
  descriptor_text = NULL;
  descriptor_text_len = 0;
  return_callback = return_cb;

  /* Canonical, h-normalized, with its BIP-380 checksum already appended — the
     same string the SD card and the QR export carry, so a card is readable by
     anything that reads those. */
  if (!descriptor_string_from_descriptor(descriptor, &descriptor_text) ||
      !descriptor_text) {
    dialog_show_error_timeout("Failed to read descriptor", return_cb, 0);
    return;
  }
  descriptor_text_len = strlen(descriptor_text);

  main_screen = theme_create_page_container(parent);
  theme_create_page_title(main_screen, "Save to NFC");

  if (encrypted) {
    /* Sealing inflates, so the size check waits until the envelope exists.
       The checksum makes a good default ID: it names this descriptor and
       nothing else. */
    char checksum[9] = {0};
    bool have_checksum =
        descriptor_checksum_from_descriptor(descriptor, checksum);
    kef_encrypt_page_create(
        parent, encrypt_return_cb, encrypt_success_cb,
        (const uint8_t *)descriptor_text, descriptor_text_len,
        have_checksum && checksum[0] ? checksum : NULL, false);
    return;
  }

  if (!payload_fits(descriptor_text_len, false))
    return;

  /* A descriptor is public data, but "public" is not "published": an unsealed
     card answers any reader that comes near it with every xpub in the wallet.
     The equivalent SD write is at least sitting in a drawer. */
  dialog_show_danger_confirm(
      "Write this descriptor unencrypted?\nAny reader can then read it.",
      plaintext_confirm_cb, NULL, DIALOG_STYLE_OVERLAY);
}

void nfc_store_descriptor_page_show(void) {
  if (main_screen)
    lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_HIDDEN);
}

void nfc_store_descriptor_page_hide(void) {
  if (main_screen)
    lv_obj_add_flag(main_screen, LV_OBJ_FLAG_HIDDEN);
}

void nfc_store_descriptor_page_destroy(void) {
  session_cleanup_unregister(nfc_store_descriptor_page_destroy);
  teardown();

  if (descriptor_text) {
    free(descriptor_text);
    descriptor_text = NULL;
  }
  descriptor_text_len = 0;

  if (main_screen) {
    lv_obj_del(main_screen);
    main_screen = NULL;
  }

  return_callback = NULL;
}
