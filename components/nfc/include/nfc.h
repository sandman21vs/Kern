/*
 * NFC card storage — KEF envelopes on ISO14443A tags
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

/** True when the tag already carries a Kern record, so callers can warn
 *  before overwriting. Absent or unreadable records report false. */
bool nfc_has_record(const nfc_tag_t *tag);

/**
 * Read the record off a tag, validating it as hostile input throughout.
 *
 * On success *data_out is a heap allocation the caller owns; wipe and free it
 * with SECURE_FREE_BUFFER(). On any failure nothing is allocated.
 */
esp_err_t nfc_read_record(const nfc_tag_t *tag, uint8_t **data_out,
                          size_t *len_out);

/** Write a record, replacing whatever was there. Refuses payloads larger than
 *  NFC_MAX_PAYLOAD or than the tag can hold. */
esp_err_t nfc_write_record(const nfc_tag_t *tag, const uint8_t *data,
                           size_t len);

/** Overwrite the record header so the tag no longer presents a record. */
esp_err_t nfc_erase(const nfc_tag_t *tag);

#ifdef __cplusplus
}
#endif

#endif /* NFC_H */
