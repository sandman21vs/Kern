/*
 * Store Descriptor on NFC — write a wallet descriptor to a card
 *
 * Same two forms the SD card already offers: a KEF envelope, or the bare
 * descriptor text. Unlike store_descriptor.c there is no storage_location_t
 * and no name prompt — a card holds one record, so there is no file to name
 * and no list to browse, and keeping the location out of this signature is
 * what keeps main/core/storage.c out of the NFC feature entirely.
 *
 * The plaintext form warns before it writes: the descriptor is every xpub in
 * the wallet, and an unsealed card hands them to any reader brought near it.
 */

#ifndef NFC_STORE_DESCRIPTOR_H
#define NFC_STORE_DESCRIPTOR_H

#include <lvgl.h>
#include <stdbool.h>

struct wally_descriptor;

/**
 * @param encrypted   true seals the descriptor in a KEF envelope; false
 *                    writes it as text, after a warning.
 * @param descriptor  borrowed for the duration of the call; the canonical
 *                    string is taken here and the handle is not retained.
 */
void nfc_store_descriptor_page_create(
    lv_obj_t *parent, void (*return_cb)(void), bool encrypted,
    const struct wally_descriptor *descriptor);
void nfc_store_descriptor_page_show(void);
void nfc_store_descriptor_page_hide(void);
void nfc_store_descriptor_page_destroy(void);

#endif /* NFC_STORE_DESCRIPTOR_H */
