/*
 * NFC Settings Page — turn card storage on or off, and probe the reader
 *
 * The toggle is what actually enables the feature: the firmware carries the
 * driver, but nothing energizes an antenna or shows a card menu until the user
 * asks for it here.
 */

#ifndef NFC_SETTINGS_H
#define NFC_SETTINGS_H

#include <lvgl.h>

void nfc_settings_page_create(lv_obj_t *parent, void (*return_cb)(void));
void nfc_settings_page_show(void);
void nfc_settings_page_hide(void);
void nfc_settings_page_destroy(void);

#endif /* NFC_SETTINGS_H */
