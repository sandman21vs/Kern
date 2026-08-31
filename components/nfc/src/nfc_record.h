/*
 * NFC card record — header layout and validation
 *
 * Every byte described below was chosen by whoever handed the user the card,
 * so parsing is an allowlist: exactly what Kern writes is accepted, anything
 * else is refused, and nothing is allocated or read until the header passes.
 *
 * Layout — 16-byte header at linear offset 0, payload immediately after:
 *   0..3    magic "KRN1"
 *   4       record type (NFC_RECORD_KEF)
 *   5       reserved, must be zero
 *   6..7    payload length, big endian
 *   8..15   reserved, must be zero
 *
 * There is no checksum: the KEF envelope is authenticated, so a half-written
 * or decaying card fails to decrypt. The payload stays untrusted regardless.
 *
 * Pure: no I/O and no ESP-IDF, so the host test suite compiles this file
 * unchanged.
 */

#ifndef NFC_RECORD_H
#define NFC_RECORD_H

#include <stddef.h>
#include <stdint.h>

#define NFC_HEADER_LEN 16
#define NFC_RECORD_MAGIC "KRN1"
#define NFC_RECORD_MAGIC_LEN 4
#define NFC_RECORD_KEF 1

/* Mirrors NFC_MAX_PAYLOAD in nfc.h; repeated so this file stays free of the
 * public header's ESP-IDF includes. The static assert in nfc.c ties them. */
#define NFC_RECORD_MAX_PAYLOAD 704

typedef struct {
  uint8_t type;
  uint16_t payload_len;
} nfc_record_t;

typedef enum {
  NFC_RECORD_OK = 0,
  NFC_RECORD_ERR_ARG,
  NFC_RECORD_ERR_MAGIC,
  NFC_RECORD_ERR_TYPE,
  NFC_RECORD_ERR_RESERVED,
  NFC_RECORD_ERR_LENGTH,
} nfc_record_err_t;

/*
 * Validate a header read off a tag.
 *
 * capacity is the tag's usable linear byte count including the header, so the
 * declared length is checked against what the card can physically hold as
 * well as against the compile-time ceiling.
 */
nfc_record_err_t nfc_record_parse(const uint8_t *header, size_t capacity,
                                  nfc_record_t *out);

/* Serialize the header for a payload of len bytes about to be written. */
nfc_record_err_t nfc_record_build(uint8_t *header, size_t len,
                                  size_t capacity);

const char *nfc_record_err_str(nfc_record_err_t err);

#endif /* NFC_RECORD_H */
