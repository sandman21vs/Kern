/*
 * Tag layer (PICC) — selection and linear byte addressing
 *
 * Presents every supported tag family as a flat byte array so the record layer
 * never learns what it is talking to. MIFARE Classic offsets skip block 0 and
 * every sector trailer; Ultralight/NTAG offsets start at page 4.
 *
 * Only tags Kern recognizes are selected at all: an unexpected SAK is reported
 * as an empty field rather than probed further.
 */

#ifndef PICC_H
#define PICC_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nfc.h"

/**
 * Wake, identify and select one tag.
 *
 * @return ESP_ERR_NOT_FOUND when the field is empty, when more than one tag is
 *         present, or when the tag is not a family Kern accepts.
 */
esp_err_t picc_select(nfc_tag_t *out);

/** Halt the tag and drop any crypto1 session. Safe to call unconditionally. */
void picc_release(void);

/**
 * Read len bytes starting at a linear offset. Offset and length are free-form;
 * the mapping handles block and page boundaries.
 */
esp_err_t picc_read(const nfc_tag_t *tag, size_t offset, uint8_t *buf,
                    size_t len);

/**
 * Write len bytes starting at a linear offset.
 *
 * offset must land on a block boundary for the tag family (16 bytes for
 * Classic, 4 for Ultralight); a trailing partial block is zero-padded. Writes
 * that would touch block 0 or a sector trailer are refused — corrupting a
 * trailer bricks its sector permanently.
 */
esp_err_t picc_write(const nfc_tag_t *tag, size_t offset, const uint8_t *buf,
                     size_t len);

#endif /* PICC_H */
