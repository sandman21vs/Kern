/*
 * NFC card storage — typed records on ISO14443A tags
 *
 * A card holds one record, tagged with what it is. Most records are KEF
 * envelopes; a wallet descriptor may instead be stored as bare text, at the
 * user's explicit choice, because a descriptor is public data by design.
 *
 * Reader: WS1850S (M5Stack RFID Unit 2) on an I2C bus the caller already
 * owns. The bus handle is passed in rather than opened here, so the component
 * carries no board dependency: every Kern BSP exposes bsp_i2c_get_handle().
 *
 * Tags: MIFARE Classic 1K/4K and Ultralight/NTAG21x. Callers never see the
 * difference — picc.c maps a linear byte offset onto whichever addressing the
 * tag family uses.
 *
 * A card is attacker-controlled input. Every routine here treats it that way:
 * see the validation notes in src/nfc_record.h.
 */

#ifndef NFC_H
#define NFC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest payload Kern will read off a card, regardless of what the card
 * claims to hold. A KEF-wrapped 24-word seed is under 100 bytes; the ceiling
 * exists so a hostile tag cannot drive a large allocation. */
#define NFC_MAX_PAYLOAD 704

/* Record types. Mirrors src/nfc_record.h, repeated so main/ never reaches into
 * the component's private headers; static asserts in nfc.c tie the two.
 *
 * The numbers are shared with the Krux fork that writes the same cards. Kern
 * writes KEF and DESCRIPTOR only — DATUM and XPUB are reserved so a card Krux
 * wrote keeps its meaning here and a later feature cannot claim the number. */
#define NFC_RECORD_TYPE_KEF 1
#define NFC_RECORD_TYPE_DESCRIPTOR 2
#define NFC_RECORD_TYPE_DATUM 3
#define NFC_RECORD_TYPE_XPUB 4

/* Readers name the types they can parse. A type buys a parser, never a
 * permission: what comes off the card still faces the validation the same
 * bytes would face arriving by QR or SD. What it does buy is that a wrong
 * card is refused at the header, before anything is allocated or decrypted. */
#define NFC_ACCEPT(t) (1u << (t))
#define NFC_ACCEPT_ANY_KNOWN 0x1Eu /* bits 1..4 */

typedef enum {
  NFC_TAG_NONE = 0,
  NFC_TAG_MIFARE_CLASSIC,
  NFC_TAG_ULTRALIGHT, /* Ultralight and NTAG21x */
} nfc_tag_type_t;

typedef struct {
  nfc_tag_type_t type;
  uint8_t uid[7];
  uint8_t uid_len; /* 4 or 7; longer UIDs are refused */
  uint8_t sak;
  size_t capacity; /* usable linear bytes, header included */
} nfc_tag_t;

/**
 * Bind the reader to an initialized I2C bus and put it in a known state.
 * Idempotent. Leaves the RF field off.
 *
 * @return ESP_ERR_NOT_FOUND when no WS1850S answers on the bus.
 */
esp_err_t nfc_init(i2c_master_bus_handle_t bus);

/** Release the reader. Turns the field off first. */
void nfc_deinit(void);

/** True once nfc_init() has succeeded. */
bool nfc_is_ready(void);

/**
 * Energize or drop the RF antenna. Off after nfc_init(); callers turn it on
 * only for as long as they are actively looking for a card.
 */
esp_err_t nfc_field_set(bool on);

/**
 * One non-blocking look for a tag in the field.
 *
 * @return ESP_OK with *out filled, ESP_ERR_NOT_FOUND when the field is empty
 *         or holds a tag whose type Kern does not accept.
 */
esp_err_t nfc_poll(nfc_tag_t *out);

/** True when the tag already carries a record of ANY known type, so callers
 *  can warn before overwriting. Deliberately not filtered: a seed about to be
 *  buried under a descriptor is something to lose either way. Absent or
 *  unreadable records report false. */
bool nfc_has_record(const nfc_tag_t *tag);

/**
 * Read the record off a tag, validating it as hostile input throughout.
 *
 * accept_mask names the types the caller can parse — NFC_ACCEPT(x), or several
 * ORed together. A record of any other type reports ESP_ERR_NOT_FOUND, exactly
 * as a blank card does, so a wrong card is indistinguishable from no card and
 * costs nothing beyond a message.
 *
 * On success *data_out is a heap allocation the caller owns; wipe and free it
 * with SECURE_FREE_BUFFER(). On any failure nothing is allocated.
 */
esp_err_t nfc_read_record(const nfc_tag_t *tag, uint32_t accept_mask,
                          uint8_t **data_out, size_t *len_out);

/** Write a record of the given type, replacing whatever was there. Refuses an
 *  unknown type, and payloads larger than NFC_MAX_PAYLOAD or than the tag can
 *  hold. */
esp_err_t nfc_write_record(const nfc_tag_t *tag, uint8_t type,
                           const uint8_t *data, size_t len);

/** Overwrite the record header so the tag no longer presents a record. */
esp_err_t nfc_erase(const nfc_tag_t *tag);

#ifdef __cplusplus
}
#endif

#endif /* NFC_H */
