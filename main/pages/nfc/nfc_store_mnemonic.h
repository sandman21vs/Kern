/*
 * Store Mnemonic on NFC — encrypt with KEF, then write to a card
 *
 * Same flow as store_mnemonic.c, with the card standing in for flash or SD.
 * Only the KEF envelope crosses the antenna; the mnemonic itself is wiped
 * before the reader is ever touched.
 */

#ifndef NFC_STORE_MNEMONIC_H
#define NFC_STORE_MNEMONIC_H

#include <lvgl.h>

void nfc_store_mnemonic_page_create(lv_obj_t *parent, void (*return_cb)(void));
void nfc_store_mnemonic_page_show(void);
void nfc_store_mnemonic_page_hide(void);
void nfc_store_mnemonic_page_destroy(void);

#endif /* NFC_STORE_MNEMONIC_H */
