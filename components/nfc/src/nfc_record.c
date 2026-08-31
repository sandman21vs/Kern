// NFC card record — header layout and validation (see nfc_record.h)

#include "nfc_record.h"

#include <string.h>

nfc_record_err_t nfc_record_parse(const uint8_t *header, size_t capacity,
                                  nfc_record_t *out) {
  if (!header || !out)
    return NFC_RECORD_ERR_ARG;

  if (memcmp(header, NFC_RECORD_MAGIC, NFC_RECORD_MAGIC_LEN) != 0)
    return NFC_RECORD_ERR_MAGIC;

  /* One known type. A record Kern did not write is not a record to grow
     lenient about. */
  if (header[4] != NFC_RECORD_KEF)
    return NFC_RECORD_ERR_TYPE;

  /* Reserved bytes mean nothing today, so zero is the only value accepted:
     it denies the field as a covert channel and stops stale bytes from
     silently acquiring meaning in a later format version. */
  if (header[5] != 0)
    return NFC_RECORD_ERR_RESERVED;
  for (size_t i = 8; i < NFC_HEADER_LEN; i++)
    if (header[i] != 0)
      return NFC_RECORD_ERR_RESERVED;

  /* The declared length is a number a stranger picked. Check it against the
     compile-time ceiling and against what this tag can physically hold before
     it is ever used to size an allocation or a read. */
  size_t len = ((size_t)header[6] << 8) | (size_t)header[7];
  if (len == 0 || len > NFC_RECORD_MAX_PAYLOAD)
    return NFC_RECORD_ERR_LENGTH;
  if (capacity < NFC_HEADER_LEN || len > capacity - NFC_HEADER_LEN)
    return NFC_RECORD_ERR_LENGTH;

  out->type = header[4];
  out->payload_len = (uint16_t)len;
  return NFC_RECORD_OK;
}

nfc_record_err_t nfc_record_build(uint8_t *header, size_t len,
                                  size_t capacity) {
  if (!header)
    return NFC_RECORD_ERR_ARG;
  if (len == 0 || len > NFC_RECORD_MAX_PAYLOAD)
    return NFC_RECORD_ERR_LENGTH;
  if (capacity < NFC_HEADER_LEN || len > capacity - NFC_HEADER_LEN)
    return NFC_RECORD_ERR_LENGTH;

  memset(header, 0, NFC_HEADER_LEN);
  memcpy(header, NFC_RECORD_MAGIC, NFC_RECORD_MAGIC_LEN);
  header[4] = NFC_RECORD_KEF;
  header[6] = (uint8_t)(len >> 8);
  header[7] = (uint8_t)(len & 0xFF);
  return NFC_RECORD_OK;
}

const char *nfc_record_err_str(nfc_record_err_t err) {
  switch (err) {
  case NFC_RECORD_OK:
    return "ok";
  case NFC_RECORD_ERR_ARG:
    return "invalid argument";
  case NFC_RECORD_ERR_MAGIC:
    return "not a Kern record";
  case NFC_RECORD_ERR_TYPE:
    return "unknown record type";
  case NFC_RECORD_ERR_RESERVED:
    return "reserved bytes not zero";
  case NFC_RECORD_ERR_LENGTH:
    return "invalid record length";
  }
  return "unknown error";
}
