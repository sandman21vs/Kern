/*
 * Load Descriptor from NFC — read a card, unseal if sealed, validate, register
 *
 * Same chain as load_descriptor_storage.c, with the card standing in for the
 * file. The card supplies either a KEF envelope or a bare descriptor string;
 * which one it is can be told without asking, and the unsealed form has to
 * prove its BIP-380 checksum — see the note in the .c.
 *
 * NFC adds a medium here, never a parser: whatever comes off the card meets
 * the same validator a descriptor arriving by QR or SD meets.
 */

#ifndef NFC_LOAD_DESCRIPTOR_H
#define NFC_LOAD_DESCRIPTOR_H

#include <lvgl.h>

void nfc_load_descriptor_page_create(lv_obj_t *parent, void (*return_cb)(void),
                                     void (*success_cb)(void));
void nfc_load_descriptor_page_show(void);
void nfc_load_descriptor_page_hide(void);
void nfc_load_descriptor_page_destroy(void);

#endif /* NFC_LOAD_DESCRIPTOR_H */
