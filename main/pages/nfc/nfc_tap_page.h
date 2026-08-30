/*
 * NFC Tap Page — "hold the card against the reader"
 *
 * The one place the RF field is energized: it comes up with the page and goes
 * down with it, so outside this page the reader is inert and the radio does
 * not exist. The reader is also attached here rather than at boot, so a device
 * with NFC switched off never touches the bus.
 *
 * The callback fires with the tag still selected, so it can read or write
 * straight away. It must not destroy this page — the owner does that.
 */

#ifndef NFC_TAP_PAGE_H
#define NFC_TAP_PAGE_H

#include <lvgl.h>
#include <nfc.h>

typedef void (*nfc_tap_cb_t)(const nfc_tag_t *tag);

/**
 * @param hint       one line under the title, e.g. "The seed will be written"
 * @param cancel_cb  user backed out, or the reader could not be reached
 * @param on_tag     a supported tag entered the field
 */
void nfc_tap_page_create(lv_obj_t *parent, const char *title, const char *hint,
                         void (*cancel_cb)(void), nfc_tap_cb_t on_tag);
void nfc_tap_page_show(void);
void nfc_tap_page_hide(void);
void nfc_tap_page_destroy(void);

#endif /* NFC_TAP_PAGE_H */
