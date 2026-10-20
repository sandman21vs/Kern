/*
 * Load Mnemonic from NFC — read a card, decrypt, confirm the key
 *
 * Same chain as load_storage.c, with one extra gate: the decrypted payload is
 * only accepted as a compact SeedQR. See the note in nfc_load_mnemonic.c.
 */

#ifndef NFC_LOAD_MNEMONIC_H
#define NFC_LOAD_MNEMONIC_H

#include <lvgl.h>

void nfc_load_mnemonic_page_create(lv_obj_t *parent, void (*return_cb)(void),
                                   void (*success_cb)(void));
void nfc_load_mnemonic_page_show(void);
void nfc_load_mnemonic_page_hide(void);
void nfc_load_mnemonic_page_destroy(void);

#endif /* NFC_LOAD_MNEMONIC_H */
