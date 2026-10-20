/*
 * NFC card record — header layout and validation
 *
 * Every byte described below was chosen by whoever handed the user the card,
 * so parsing is an allowlist: exactly what Kern writes is accepted, anything
 * else is refused, and nothing is allocated or read until the header passes.
 *
 * Layout — 16-byte header at linear offset 0, payload immediately after:
 *   0..3    magic "KRN1"
 *   4       record type (NFC_RECORD_KEF, NFC_RECORD_DESCRIPTOR, ...)
 *   5       reserved, must be zero
 *   6..7    payload length, big endian
 *   8..15   reserved, must be zero
 *
 * The record layer carries no checksum. For a sealed payload none is needed:
 * the KEF envelope is authenticated, so a half-written or decaying card fails
 * to decrypt. A NFC_RECORD_DESCRIPTOR payload may instead be a bare descriptor
 * string, which brings its own BIP-380 checksum — verified one layer up, in
 * main/pages/nfc/nfc_load_descriptor.c, because nothing down here knows what
 * the bytes are meant to be. The payload stays untrusted either way.
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

/* The magic tags the format, not the device, so these numbers are shared with
 * the Krux fork that writes the same cards. Kern writes 1 and 2 only; 3 and 4
 * are reserved rather than free, so a card Krux wrote keeps its meaning and a
 * later Kern feature cannot claim a number out from under it. */
#define NFC_RECORD_KEF 1        /* KEF envelope (seed backup) */
#define NFC_RECORD_DESCRIPTOR 2 /* wallet descriptor, sealed or plaintext */
#define NFC_RECORD_DATUM 3      /* reserved: Krux writes it, Kern does not */
#define NFC_RECORD_XPUB 4       /* reserved: Krux writes it, Kern does not */

/* Callers name the types they can parse as a bitmask. A uint32_t cannot
 * express a type above 31, which is the discipline this format wants: the type
 * byte is a small allowlist shared with another firmware, and one that stays
 * enumerable is one that stays reviewable. */
#define NFC_RECORD_BIT(t) (1u << (t))
#define NFC_RECORD_MASK_ANY_KNOWN                                              \
  (NFC_RECORD_BIT(NFC_RECORD_KEF) | NFC_RECORD_BIT(NFC_RECORD_DESCRIPTOR) |    \
   NFC_RECORD_BIT(NFC_RECORD_DATUM) | NFC_RECORD_BIT(NFC_RECORD_XPUB))

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
 *
 * accept_mask names the types the caller can parse, built from
 * NFC_RECORD_BIT(). A caller about to parse a payload names one type, so a
 * record of another kind is refused before anything is read — a descriptor
 * card offered to the mnemonic loader reads as no record at all. Asking
 * whether there is anything here to overwrite is the other question, and it
 * passes NFC_RECORD_MASK_ANY_KNOWN. The mask is intersected with that set, so
 * a caller cannot widen the allowlist past what this file knows.
 *
 * Checks run magic, then type, then reserved, then length, and stop at the
 * first divergence; nothing is measured against the payload until the header
 * has earned it.
 */
nfc_record_err_t nfc_record_parse(const uint8_t *header, size_t capacity,
                                  uint32_t accept_mask, nfc_record_t *out);

/* Serialize the header for a payload of len bytes about to be written.
 * An unknown type is refused rather than stamped. */
nfc_record_err_t nfc_record_build(uint8_t *header, size_t len, size_t capacity,
                                  uint8_t type);

const char *nfc_record_err_str(nfc_record_err_t err);

#endif /* NFC_RECORD_H */
