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
 *   8..11   CRC-32 of the payload, big endian
 *   12..15  reserved, must be zero
 *
 * The CRC catches a half-written or decaying card. It is not a tamper check —
 * authentication is the KEF envelope's job, and the payload stays untrusted
 * even after it verifies.
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
  uint32_t payload_crc;
} nfc_record_t;

typedef enum {
  NFC_RECORD_OK = 0,
  NFC_RECORD_ERR_ARG,
  NFC_RECORD_ERR_MAGIC,
  NFC_RECORD_ERR_TYPE,
  NFC_RECORD_ERR_RESERVED,
  NFC_RECORD_ERR_LENGTH,
  NFC_RECORD_ERR_CRC,
} nfc_record_err_t;

/* CRC-32/ISO-HDLC, computed bitwise — no table, negligible over 704 bytes. */
uint32_t nfc_record_crc32(const uint8_t *data, size_t len);

/*
 * Validate a header read off a tag.
 *
 * capacity is the tag's usable linear byte count including the header, so the
 * declared length is checked against what the card can physically hold as
 * well as against the compile-time ceiling.
 */
nfc_record_err_t nfc_record_parse(const uint8_t *header, size_t capacity,
                                  nfc_record_t *out);

/* Confirm a payload matches the CRC its header promised. */
nfc_record_err_t nfc_record_verify(const nfc_record_t *rec,
                                   const uint8_t *payload, size_t len);

/* Serialize the header for a payload about to be written. */
nfc_record_err_t nfc_record_build(uint8_t *header, const uint8_t *payload,
                                  size_t len, size_t capacity);

const char *nfc_record_err_str(nfc_record_err_t err);

#endif /* NFC_RECORD_H */
