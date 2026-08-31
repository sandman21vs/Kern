// Tag layer — selection and linear byte addressing (see picc.h)

#include "picc.h"

#include <string.h>

#include "esp_log.h"
#include "nfc_record.h"
#include "pcd_ws1850s.h"

static const char *TAG = "NFC_PICC";

/* SAK values Kern accepts. Anything else is treated as an empty field: the
 * point is to talk only to what we know how to talk to. */
#define SAK_CLASSIC_1K 0x08
#define SAK_CLASSIC_4K 0x18
#define SAK_CLASSIC_1K_INFINEON 0x88
#define SAK_ULTRALIGHT 0x00
#define SAK_CASCADE_BIT 0x04

#define CASCADE_TAG 0x88

/* MIFARE Classic geometry. 4K tags are addressed as 1K: their upper sectors
 * hold 16 blocks instead of 4, and a seed needs a fraction of the first 16
 * sectors anyway. */
#define MF_BLOCK_SIZE 16
#define MF_SECTORS 16
#define MF_DATA_BLOCKS 47 /* 64 blocks less block 0 and 16 trailers */
static const uint8_t MF_DEFAULT_KEY[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Ultralight / NTAG geometry */
#define UL_PAGE_SIZE 4
#define UL_DATA_FIRST_PAGE 4
#define UL_CC_PAGE 3
#define UL_MIN_CAPACITY 48  /* plain Ultralight, pages 4..15 */
#define UL_MAX_CAPACITY 888 /* NTAG216 user memory */

/* Nothing larger than one record is ever addressable, whatever a tag claims. */
#define PICC_MAX_CAPACITY (NFC_MAX_PAYLOAD + NFC_HEADER_LEN)

/* Sector currently authenticated, or -1. Reset on every select and release so
 * a stale session can never be mistaken for a fresh one. */
static int authed_sector = -1;

/* ---------- Selection ---------- */

static esp_err_t request_tag(void) {
  uint8_t atqa[2];
  size_t len = sizeof(atqa);

  /* WUPA rather than REQA, and it is a 7-bit frame. Every select starts by
     releasing whatever was held, which halts the tag — and a halted tag
     answers WUPA but ignores REQA, so REQA here would make a card
     unselectable for the rest of its time in the field. */
  const uint8_t cmd = PICC_CMD_WUPA;
  esp_err_t ret = pcd_transceive(&cmd, 1, 7, atqa, &len, NULL);
  if (ret != ESP_OK)
    return ret;
  if (len != 2)
    return ESP_ERR_INVALID_RESPONSE;
  return ESP_OK;
}

/*
 * One cascade level: anticollision to learn four UID bytes, then select.
 * No collision resolution — Kern asks for a single card, and two cards in the
 * field simply read as "nothing there" until one is taken away.
 */
static esp_err_t cascade_level(uint8_t sel_cmd, uint8_t uid_part[4],
                               uint8_t *sak_out) {
  uint8_t reply[5];
  size_t len = sizeof(reply);

  const uint8_t anticoll[2] = {sel_cmd, 0x20};
  esp_err_t ret =
      pcd_transceive(anticoll, sizeof(anticoll), 0, reply, &len, NULL);
  if (ret != ESP_OK)
    return ret;
  if (len != 5)
    return ESP_ERR_INVALID_RESPONSE;

  /* BCC is a plain XOR check. A mismatch means a malformed frame, so stop
     rather than build a UID out of it. */
  uint8_t bcc = reply[0] ^ reply[1] ^ reply[2] ^ reply[3];
  if (bcc != reply[4])
    return ESP_ERR_INVALID_CRC;

  memcpy(uid_part, reply, 4);

  uint8_t select[7] = {sel_cmd,  0x70,     reply[0], reply[1],
                       reply[2], reply[3], reply[4]};
  uint8_t sak[3];
  size_t sak_len = sizeof(sak);
  ret = pcd_transceive_crc(select, sizeof(select), sak, &sak_len);
  if (ret != ESP_OK)
    return ret;
  if (sak_len != 1)
    return ESP_ERR_INVALID_RESPONSE;

  *sak_out = sak[0];
  return ESP_OK;
}

static esp_err_t ultralight_capacity(size_t *out) {
  /* A READ returns four pages; the compatibility container sits in page 3. */
  uint8_t pages[16 + PCD_CRC_LEN];
  size_t len = sizeof(pages);
  const uint8_t cmd[2] = {PICC_CMD_MF_READ, UL_CC_PAGE};

  esp_err_t ret = pcd_transceive_crc(cmd, sizeof(cmd), pages, &len);
  if (ret != ESP_OK)
    return ret;
  if (len != 16)
    return ESP_ERR_INVALID_RESPONSE;

  /* pages[2] is the size byte, and it was written by whoever held the tag
     last — `size * 8` is exactly the kind of number that turns into an
     overflow if believed. Take it only when the NFC Forum magic byte is
     there, and clamp it at both ends regardless. */
  size_t cap = UL_MIN_CAPACITY;
  if (pages[0] == 0xE1 && pages[2] > 0)
    cap = (size_t)pages[2] * 8;

  if (cap < UL_MIN_CAPACITY)
    cap = UL_MIN_CAPACITY;
  if (cap > UL_MAX_CAPACITY)
    cap = UL_MAX_CAPACITY;

  *out = cap;
  return ESP_OK;
}

esp_err_t picc_select(nfc_tag_t *out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  if (!pcd_is_ready())
    return ESP_ERR_INVALID_STATE;

  picc_release();
  memset(out, 0, sizeof(*out));

  if (request_tag() != ESP_OK)
    return ESP_ERR_NOT_FOUND;

  uint8_t part[4];
  uint8_t sak = 0;
  if (cascade_level(PICC_CMD_SEL_CL1, part, &sak) != ESP_OK)
    return ESP_ERR_NOT_FOUND;

  if (sak & SAK_CASCADE_BIT) {
    /* Double-size UID: the first byte of level 1 is the cascade tag, not UID
       data. Level 3 (ten-byte UIDs) is not supported and is not guessed at. */
    if (part[0] != CASCADE_TAG)
      return ESP_ERR_NOT_FOUND;
    memcpy(out->uid, &part[1], 3);

    if (cascade_level(PICC_CMD_SEL_CL2, part, &sak) != ESP_OK)
      return ESP_ERR_NOT_FOUND;
    if (sak & SAK_CASCADE_BIT)
      return ESP_ERR_NOT_FOUND;

    memcpy(&out->uid[3], part, 4);
    out->uid_len = 7;
  } else {
    memcpy(out->uid, part, 4);
    out->uid_len = 4;
  }

  out->sak = sak;

  switch (sak) {
  case SAK_CLASSIC_1K:
  case SAK_CLASSIC_4K:
  case SAK_CLASSIC_1K_INFINEON:
    out->type = NFC_TAG_MIFARE_CLASSIC;
    out->capacity = MF_DATA_BLOCKS * MF_BLOCK_SIZE;
    break;
  case SAK_ULTRALIGHT:
    out->type = NFC_TAG_ULTRALIGHT;
    if (ultralight_capacity(&out->capacity) != ESP_OK) {
      picc_release();
      return ESP_ERR_NOT_FOUND;
    }
    break;
  default:
    ESP_LOGD(TAG, "Unsupported SAK 0x%02x", sak);
    picc_release();
    return ESP_ERR_NOT_FOUND;
  }

  if (out->capacity > PICC_MAX_CAPACITY)
    out->capacity = PICC_MAX_CAPACITY;

  return ESP_OK;
}

void picc_release(void) {
  authed_sector = -1;
  if (!pcd_is_ready())
    return;

  /* HALT goes out before crypto is dropped: while a sector is authenticated
     the reader enciphers the frame, and a plaintext HALT would be ignored,
     leaving the tag awake in a state it thinks is still authenticated.
     HALT draws no reply, so a timeout here is the success case. */
  uint8_t halt[2] = {PICC_CMD_HALT, 0x00};
  uint8_t crc[PCD_CRC_LEN];
  if (pcd_calc_crc(halt, sizeof(halt), crc) == ESP_OK) {
    uint8_t frame[4] = {halt[0], halt[1], crc[0], crc[1]};
    (void)pcd_transceive(frame, sizeof(frame), 0, NULL, NULL, NULL);
  }

  if (pcd_crypto_active())
    pcd_stop_crypto();
}

/* ---------- MIFARE Classic ---------- */

/*
 * Map a data-block index onto a physical block, skipping the manufacturer
 * block and every sector trailer. Sector 0 contributes two data blocks
 * (1, 2); every later sector contributes three.
 */
static esp_err_t mf_physical_block(size_t index, uint8_t *block_out) {
  if (index >= MF_DATA_BLOCKS)
    return ESP_ERR_INVALID_SIZE;

  size_t block;
  if (index < 2) {
    block = index + 1;
  } else {
    size_t rest = index - 2;
    block = (rest / 3 + 1) * 4 + (rest % 3);
  }

  /* The mapping already excludes them; this catches a future edit to the
     arithmetic before it destroys a sector rather than after. */
  if (block == 0 || (block % 4) == 3)
    return ESP_ERR_INVALID_STATE;

  *block_out = (uint8_t)block;
  return ESP_OK;
}

static esp_err_t mf_authenticate(const nfc_tag_t *tag, uint8_t block) {
  int sector = block / 4;
  if (sector == authed_sector)
    return ESP_OK;

  esp_err_t ret = pcd_mf_authenticate(PICC_CMD_MF_AUTH_KEY_A, block,
                                      MF_DEFAULT_KEY, tag->uid, tag->uid_len);
  if (ret != ESP_OK) {
    authed_sector = -1;
    return ret;
  }

  authed_sector = sector;
  return ESP_OK;
}

static esp_err_t mf_read_block(const nfc_tag_t *tag, uint8_t block,
                               uint8_t out[MF_BLOCK_SIZE]) {
  esp_err_t ret = mf_authenticate(tag, block);
  if (ret != ESP_OK)
    return ret;

  /* The block arrives with its CRC_A attached; the buffer has to hold both or
     the reply reads as oversized. */
  uint8_t buf[MF_BLOCK_SIZE + PCD_CRC_LEN];
  size_t len = sizeof(buf);
  const uint8_t cmd[2] = {PICC_CMD_MF_READ, block};
  ret = pcd_transceive_crc(cmd, sizeof(cmd), buf, &len);
  if (ret != ESP_OK)
    return ret;
  if (len != MF_BLOCK_SIZE)
    return ESP_ERR_INVALID_RESPONSE;

  memcpy(out, buf, MF_BLOCK_SIZE);
  return ESP_OK;
}

/* MIFARE write is two frames, each answered by a 4-bit ACK. */
static esp_err_t mf_frame_ack(const uint8_t *data, size_t len) {
  if (len + 2 > PCD_FIFO_SIZE)
    return ESP_ERR_INVALID_ARG;

  uint8_t frame[PCD_FIFO_SIZE];
  memcpy(frame, data, len);
  esp_err_t ret = pcd_calc_crc(data, len, &frame[len]);
  if (ret != ESP_OK)
    return ret;

  uint8_t reply = 0;
  size_t reply_len = 1;
  uint8_t valid_bits = 0;
  ret = pcd_transceive(frame, len + 2, 0, &reply, &reply_len, &valid_bits);
  if (ret != ESP_OK)
    return ret;

  /* An ACK is exactly one nibble holding 0x0A. Anything else — a NAK, a
     full byte, a longer frame — is a failed write, not a partial success. */
  if (reply_len != 1 || valid_bits != 4 || (reply & 0x0F) != 0x0A)
    return ESP_ERR_INVALID_RESPONSE;
  return ESP_OK;
}

static esp_err_t mf_write_block(const nfc_tag_t *tag, uint8_t block,
                                const uint8_t data[MF_BLOCK_SIZE]) {
  esp_err_t ret = mf_authenticate(tag, block);
  if (ret != ESP_OK)
    return ret;

  const uint8_t cmd[2] = {PICC_CMD_MF_WRITE, block};
  ret = mf_frame_ack(cmd, sizeof(cmd));
  if (ret != ESP_OK)
    return ret;

  return mf_frame_ack(data, MF_BLOCK_SIZE);
}

/* ---------- Ultralight / NTAG ---------- */

static esp_err_t ul_read_pages(uint8_t page, uint8_t out[16]) {
  uint8_t buf[16 + PCD_CRC_LEN];
  size_t len = sizeof(buf);
  const uint8_t cmd[2] = {PICC_CMD_MF_READ, page};

  esp_err_t ret = pcd_transceive_crc(cmd, sizeof(cmd), buf, &len);
  if (ret != ESP_OK)
    return ret;
  if (len != 16)
    return ESP_ERR_INVALID_RESPONSE;

  memcpy(out, buf, 16);
  return ESP_OK;
}

static esp_err_t ul_write_page(uint8_t page, const uint8_t data[4]) {
  uint8_t cmd[6] = {PICC_CMD_UL_WRITE, page,    data[0],
                    data[1],           data[2], data[3]};
  return mf_frame_ack(cmd, sizeof(cmd));
}

/* ---------- Linear access ---------- */

static esp_err_t check_range(const nfc_tag_t *tag, size_t offset, size_t len) {
  if (!tag || len == 0)
    return ESP_ERR_INVALID_ARG;
  if (offset > tag->capacity || len > tag->capacity - offset)
    return ESP_ERR_INVALID_SIZE;
  return ESP_OK;
}

esp_err_t picc_read(const nfc_tag_t *tag, size_t offset, uint8_t *buf,
                    size_t len) {
  if (!buf)
    return ESP_ERR_INVALID_ARG;
  esp_err_t ret = check_range(tag, offset, len);
  if (ret != ESP_OK)
    return ret;

  const size_t chunk =
      (tag->type == NFC_TAG_MIFARE_CLASSIC) ? MF_BLOCK_SIZE : 16;
  size_t done = 0;
  while (done < len) {
    size_t pos = offset + done;
    size_t aligned = pos - (pos % chunk);
    size_t skip = pos - aligned;
    size_t take = chunk - skip;
    if (take > len - done)
      take = len - done;

    uint8_t block[16];
    if (tag->type == NFC_TAG_MIFARE_CLASSIC) {
      uint8_t phys = 0;
      ret = mf_physical_block(aligned / MF_BLOCK_SIZE, &phys);
      if (ret != ESP_OK)
        return ret;
      ret = mf_read_block(tag, phys, block);
    } else {
      uint8_t page = (uint8_t)(UL_DATA_FIRST_PAGE + aligned / UL_PAGE_SIZE);
      ret = ul_read_pages(page, block);
    }
    if (ret != ESP_OK)
      return ret;

    memcpy(buf + done, block + skip, take);
    done += take;
  }

  return ESP_OK;
}

esp_err_t picc_write(const nfc_tag_t *tag, size_t offset, const uint8_t *buf,
                     size_t len) {
  if (!buf)
    return ESP_ERR_INVALID_ARG;
  esp_err_t ret = check_range(tag, offset, len);
  if (ret != ESP_OK)
    return ret;

  const size_t unit =
      (tag->type == NFC_TAG_MIFARE_CLASSIC) ? MF_BLOCK_SIZE : UL_PAGE_SIZE;
  if (offset % unit != 0)
    return ESP_ERR_INVALID_ARG;

  size_t done = 0;
  while (done < len) {
    /* Pad the tail so a short final block still writes a full unit; the
       record header carries the real length. */
    uint8_t unit_buf[MF_BLOCK_SIZE] = {0};
    size_t take = len - done;
    if (take > unit)
      take = unit;
    memcpy(unit_buf, buf + done, take);

    size_t pos = offset + done;
    if (tag->type == NFC_TAG_MIFARE_CLASSIC) {
      uint8_t phys = 0;
      ret = mf_physical_block(pos / MF_BLOCK_SIZE, &phys);
      if (ret != ESP_OK)
        return ret;
      ret = mf_write_block(tag, phys, unit_buf);
    } else {
      uint8_t page = (uint8_t)(UL_DATA_FIRST_PAGE + pos / UL_PAGE_SIZE);
      ret = ul_write_page(page, unit_buf);
    }
    if (ret != ESP_OK)
      return ret;

    done += unit;
  }

  return ESP_OK;
}
