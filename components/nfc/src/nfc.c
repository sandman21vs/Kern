// NFC card storage — reader lifecycle and record I/O (see nfc.h)

#include "nfc.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nfc_record.h"
#include "pcd_ws1850s.h"
#include "picc.h"

static const char *TAG = "NFC";

_Static_assert(NFC_MAX_PAYLOAD == NFC_RECORD_MAX_PAYLOAD,
               "public and record-layer payload ceilings must agree");

/* Local wipe rather than main/utils/secure_mem.h: components do not depend on
 * main/. Same technique — a volatile function pointer the compiler cannot
 * optimize into a dead store. */
static void *(*const volatile wipe_fn)(void *, int, size_t) = memset;

static void wipe(void *ptr, size_t len) {
  if (ptr && len > 0)
    wipe_fn(ptr, 0, len);
}

static esp_err_t record_err_to_esp(nfc_record_err_t err) {
  switch (err) {
  case NFC_RECORD_OK:
    return ESP_OK;
  case NFC_RECORD_ERR_ARG:
    return ESP_ERR_INVALID_ARG;
  case NFC_RECORD_ERR_LENGTH:
    return ESP_ERR_INVALID_SIZE;
  case NFC_RECORD_ERR_CRC:
    return ESP_ERR_INVALID_CRC;
  case NFC_RECORD_ERR_MAGIC:
  case NFC_RECORD_ERR_TYPE:
  case NFC_RECORD_ERR_RESERVED:
    return ESP_ERR_NOT_FOUND;
  }
  return ESP_FAIL;
}

/* ---------- Lifecycle ---------- */

esp_err_t nfc_init(i2c_master_bus_handle_t bus) { return pcd_init(bus); }

void nfc_deinit(void) {
  picc_release();
  pcd_deinit();
}

bool nfc_is_ready(void) { return pcd_is_ready(); }

esp_err_t nfc_field_set(bool on) {
  if (!pcd_is_ready())
    return ESP_ERR_INVALID_STATE;
  if (!on)
    picc_release();
  return pcd_antenna(on);
}

esp_err_t nfc_poll(nfc_tag_t *out) { return picc_select(out); }

/* ---------- Records ---------- */

/* Read and validate the header. Nothing downstream runs until this passes. */
static esp_err_t read_header(const nfc_tag_t *tag, nfc_record_t *rec) {
  uint8_t header[NFC_HEADER_LEN];
  esp_err_t ret = picc_read(tag, 0, header, sizeof(header));
  if (ret != ESP_OK) {
    /* Logged rather than swallowed: a card that selects but will not read is
       the failure worth seeing on the console, and it looks identical to a
       blank card from the UI. */
    ESP_LOGW(TAG, "Header read failed: %s", esp_err_to_name(ret));
    return ret;
  }

  nfc_record_err_t err = nfc_record_parse(header, tag->capacity, rec);
  if (err != NFC_RECORD_OK) {
    ESP_LOGW(TAG, "Header rejected: %s", nfc_record_err_str(err));
    return record_err_to_esp(err);
  }
  return ESP_OK;
}

bool nfc_has_record(const nfc_tag_t *tag) {
  if (!tag)
    return false;
  nfc_record_t rec;
  return read_header(tag, &rec) == ESP_OK;
}

esp_err_t nfc_read_record(const nfc_tag_t *tag, uint8_t **data_out,
                          size_t *len_out) {
  if (!tag || !data_out || !len_out)
    return ESP_ERR_INVALID_ARG;

  *data_out = NULL;
  *len_out = 0;

  nfc_record_t rec;
  esp_err_t ret = read_header(tag, &rec);
  if (ret != ESP_OK)
    return ret;

  /* rec.payload_len has already been bounded by both the compile-time ceiling
     and this tag's capacity, so it is safe to allocate against. */
  uint8_t *payload = malloc(rec.payload_len);
  if (!payload)
    return ESP_ERR_NO_MEM;

  ret = picc_read(tag, NFC_HEADER_LEN, payload, rec.payload_len);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Payload read failed at %u bytes: %s",
             (unsigned)rec.payload_len, esp_err_to_name(ret));
    wipe(payload, rec.payload_len);
    free(payload);
    return ret;
  }

  nfc_record_err_t err = nfc_record_verify(&rec, payload, rec.payload_len);
  if (err != NFC_RECORD_OK) {
    /* Could be a half-written card or a hostile one. Either way the bytes may
       be ciphertext, so they leave no copy behind. */
    wipe(payload, rec.payload_len);
    free(payload);
    ESP_LOGW(TAG, "Payload rejected: %s", nfc_record_err_str(err));
    return record_err_to_esp(err);
  }

  *data_out = payload;
  *len_out = rec.payload_len;
  return ESP_OK;
}

esp_err_t nfc_write_record(const nfc_tag_t *tag, const uint8_t *data,
                           size_t len) {
  if (!tag || !data || len == 0)
    return ESP_ERR_INVALID_ARG;
  if (len > NFC_MAX_PAYLOAD)
    return ESP_ERR_INVALID_SIZE;

  uint8_t header[NFC_HEADER_LEN];
  nfc_record_err_t err = nfc_record_build(header, data, len, tag->capacity);
  if (err != NFC_RECORD_OK)
    return record_err_to_esp(err);

  /* One contiguous image keeps the write block-aligned from offset zero, so
     picc_write never has to touch a block it does not fully own. */
  size_t total = NFC_HEADER_LEN + len;
  uint8_t *image = calloc(1, total);
  if (!image)
    return ESP_ERR_NO_MEM;

  memcpy(image, header, NFC_HEADER_LEN);
  memcpy(image + NFC_HEADER_LEN, data, len);

  esp_err_t ret = picc_write(tag, 0, image, total);

  wipe(image, total);
  free(image);
  return ret;
}

esp_err_t nfc_erase(const nfc_tag_t *tag) {
  if (!tag)
    return ESP_ERR_INVALID_ARG;

  /* Zeroing the header is enough to stop the tag presenting a record: the
     magic no longer matches and parsing stops at the first check. */
  uint8_t blank[NFC_HEADER_LEN] = {0};
  return picc_write(tag, 0, blank, sizeof(blank));
}
