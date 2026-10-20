/*
 * PN532 reader (Elechouse "PN532 NFC RFID V3" and clones) over I2C
 *
 * The PN532 is not an MFRC522 on another bus: between host and radio there is
 * firmware, driven by framed commands.
 *
 *   host   00 00 FF LEN LCS D4 cmd ... DCS 00
 *   chip   00 00 FF 00 FF 00                      (ACK)
 *   chip   00 00 FF LEN LCS D5 cmd+1 ... DCS 00
 *
 * Over I2C every read starts with a status byte, 0x01 once a frame is ready,
 * and the frame has to be read in one go.
 *
 * That firmware can select cards and run MIFARE by itself. It is not asked
 * to: this backend hands picc.c raw frames exactly as the WS1850S does, so
 * selection, addressing and every bound on tag input stay where they were.
 * Three things are used from the firmware:
 *
 *   InCommunicateThru    exchange one raw frame with the tag
 *   Read/WriteRegister   the radio's registers (the CIU, a PN512 with the
 *                        MFRC522 map) for last bits and crypto1
 *   RFConfiguration      field and reply deadline
 *
 * CRC_A is computed here rather than by the chip, so the frame on air is
 * exactly the one the tag layer built. Ported from the Specter-DIY ESP32-P4
 * fork, which measured it against MIFARE Classic and a JavaCard; the command
 * sequence is the one libnfc uses for raw frames.
 *
 * Wiring: the module's mode switch on I2C, supply at 3.3 V — its pull-ups tie
 * SDA and SCL to its own VCC, and the bus is shared with touch and camera.
 */

#include "pcd.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "NFC_PN532";

#define I2C_ADDR CONFIG_KERN_NFC_PN532_I2C_ADDR
#define I2C_TIMEOUT_MS CONFIG_KERN_NFC_I2C_TIMEOUT_MS

/* Firmware commands */
#define CMD_FIRMWARE_VERSION 0x02
#define CMD_READ_REGISTER 0x06
#define CMD_WRITE_REGISTER 0x08
#define CMD_SET_PARAMETERS 0x12
#define CMD_SAM_CONFIGURATION 0x14
#define CMD_RF_CONFIGURATION 0x32
#define CMD_COMMUNICATE_THRU 0x42

#define TFI_HOST_TO_CHIP 0xD4
#define TFI_CHIP_TO_HOST 0xD5
#define IC_PN532 0x32

static const uint8_t ACK_FRAME[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

/* CIU registers: the MFRC522 map at 0x63xx. Page 0 sits at 0x6330 + reg,
   pages 1 and 2 at 0x6300 + (reg - 0x10). */
#define CIU_TX_MODE 0x6302
#define CIU_RX_MODE 0x6303
#define CIU_TX_AUTO 0x6305
#define CIU_MANUAL_RCV 0x630D
#define CIU_COMMAND 0x6331
#define CIU_STATUS2 0x6338
#define CIU_FIFO_DATA 0x6339
#define CIU_FIFO_LEVEL 0x633A
#define CIU_CONTROL 0x633C
#define CIU_BIT_FRAMING 0x633D

#define CIU_CMD_IDLE 0x00
#define CIU_CMD_MF_AUTHENT 0x0E
#define STATUS2_CRYPTO1_ON 0x08

/* InCommunicateThru status, low six bits */
#define STATUS_ERR_MASK 0x3F
#define STATUS_ERR_TIMEOUT 0x01

/* Tag reply deadline: the firmware takes 100 us * 2^(n-1), n in 1..16.
   n = 10 is 51.2 ms — covers a MIFARE block write, and stops a mute tag from
   holding a read. */
#define TIMEOUT_CODE 0x0A
#define TIMEOUT_CODE_MS 52

/* How long the host waits on the firmware beyond the tag deadline, and for a
   command that never touches the radio (registers, configuration). */
#define COMMAND_MARGIN_MS 60
#define LOCAL_TIMEOUT_MS 60

/* Largest command parameter block sent: MIFARE authentication writes 15
   registers in one go, three bytes each. A raw frame is at most a FIFO. */
#define MAX_PARAMS 64
/* Largest reply data accepted: the InCommunicateThru status byte plus a full
   FIFO. Anything bigger is refused before it is copied. */
#define MAX_REPLY (1 + PCD_FIFO_SIZE)

static i2c_master_dev_handle_t dev = NULL;
static bool ready = false;
static bool crypto_on = false;
/* CIU_BitFraming as last written, or -1 when unknown */
static int tx_bits_cached = -1;

/* Local wipe, as in nfc.c: a volatile function pointer the compiler cannot
   optimize into a dead store. The MIFARE key passes through these buffers. */
static void *(*const volatile wipe_fn)(void *, int, size_t) = memset;

static void pn_deinit(void);
static esp_err_t pn_antenna(bool on);

/* ---------- Firmware frames ---------- */

/* Poll the status byte until the chip has a frame for us. A busy chip may
   NACK its address, so a failed transfer is a "not yet", not an error. */
static esp_err_t wait_ready(uint32_t timeout_ms) {
  const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
  do {
    uint8_t status = 0;
    if (i2c_master_receive(dev, &status, 1, I2C_TIMEOUT_MS) == ESP_OK &&
        (status & 0x01))
      return ESP_OK;
    vTaskDelay(1);
  } while (esp_timer_get_time() < deadline);
  return ESP_ERR_TIMEOUT;
}

/*
 * Send one command and return the data of its answer (after D5 cmd+1).
 * reply_cap bounds what is accepted; a larger answer is refused, not cut.
 */
static esp_err_t command(uint8_t cmd, const uint8_t *params, size_t plen,
                         uint8_t *reply, size_t reply_cap, size_t *reply_len,
                         uint32_t timeout_ms) {
  if (!dev || plen > MAX_PARAMS || reply_cap > MAX_REPLY)
    return ESP_ERR_INVALID_ARG;

  /* Preamble, start code, LEN, LCS, TFI, cmd, params, DCS, postamble */
  uint8_t frame[5 + 2 + MAX_PARAMS + 2];
  const uint8_t len = (uint8_t)(2 + plen);
  size_t n = 0;
  frame[n++] = 0x00;
  frame[n++] = 0x00;
  frame[n++] = 0xFF;
  frame[n++] = len;
  frame[n++] = (uint8_t)(0x100 - len);
  frame[n++] = TFI_HOST_TO_CHIP;
  frame[n++] = cmd;
  uint8_t sum = TFI_HOST_TO_CHIP + cmd;
  for (size_t i = 0; i < plen; i++) {
    frame[n++] = params[i];
    sum += params[i];
  }
  frame[n++] = (uint8_t)(0x100 - sum);
  frame[n++] = 0x00;

  esp_err_t ret = i2c_master_transmit(dev, frame, n, I2C_TIMEOUT_MS);
  wipe_fn(frame, 0, sizeof(frame));
  if (ret != ESP_OK)
    return ESP_FAIL;

  /* The ACK says only that the frame arrived whole */
  ret = wait_ready(LOCAL_TIMEOUT_MS);
  if (ret != ESP_OK)
    return ret;
  uint8_t ack[1 + sizeof(ACK_FRAME)];
  if (i2c_master_receive(dev, ack, sizeof(ack), I2C_TIMEOUT_MS) != ESP_OK)
    return ESP_FAIL;
  if (memcmp(&ack[1], ACK_FRAME, sizeof(ACK_FRAME)) != 0)
    return ESP_ERR_INVALID_RESPONSE;

  ret = wait_ready(timeout_ms);
  if (ret != ESP_OK)
    return ret;

  /* Status, preamble, start code, LEN, LCS, TFI, cmd+1, data, DCS,
     postamble. Reading past the frame end is harmless: the chip pads. */
  uint8_t raw[1 + 5 + 2 + MAX_REPLY + 2];
  const size_t raw_len = 1 + 5 + 2 + reply_cap + 2;
  if (i2c_master_receive(dev, raw, raw_len, I2C_TIMEOUT_MS) != ESP_OK)
    return ESP_FAIL;

  ret = ESP_ERR_INVALID_RESPONSE;
  if (!(raw[0] & 0x01) || raw[1] != 0x00 || raw[2] != 0x00 || raw[3] != 0xFF)
    goto out;
  const uint8_t flen = raw[4];
  if ((uint8_t)(flen + raw[5]) != 0)
    goto out;
  /* A length the chip chose: bound it before it is used as one. An error
     frame from the firmware is a single 0x7F byte, below the minimum. */
  if (flen < 2 || flen > 2 + reply_cap) {
    ret = flen > 2 + reply_cap ? ESP_ERR_INVALID_SIZE : ret;
    goto out;
  }
  const uint8_t *body = &raw[6];
  sum = 0;
  for (size_t i = 0; i < flen; i++)
    sum += body[i];
  if ((uint8_t)(sum + body[flen]) != 0)
    goto out;
  if (body[0] != TFI_CHIP_TO_HOST || body[1] != (uint8_t)(cmd + 1))
    goto out;

  if (reply && reply_len) {
    memcpy(reply, &body[2], flen - 2);
    *reply_len = flen - 2;
  }
  ret = ESP_OK;

out:
  wipe_fn(raw, 0, sizeof(raw));
  return ret;
}

/* ---------- Radio registers ---------- */

typedef struct {
  uint16_t reg;
  uint8_t val;
} reg_val_t;

/* Write CIU registers, in order, in one command */
static esp_err_t write_regs(const reg_val_t *regs, size_t count) {
  uint8_t params[MAX_PARAMS];
  if (count * 3 > sizeof(params))
    return ESP_ERR_INVALID_ARG;
  for (size_t i = 0; i < count; i++) {
    params[3 * i] = (uint8_t)(regs[i].reg >> 8);
    params[3 * i + 1] = (uint8_t)regs[i].reg;
    params[3 * i + 2] = regs[i].val;
  }
  uint8_t reply[1];
  size_t reply_len = 0;
  esp_err_t ret = command(CMD_WRITE_REGISTER, params, count * 3, reply,
                          sizeof(reply), &reply_len, LOCAL_TIMEOUT_MS);
  wipe_fn(params, 0, sizeof(params));
  return ret;
}

static esp_err_t write_reg(uint16_t reg, uint8_t val) {
  const reg_val_t rv = {reg, val};
  return write_regs(&rv, 1);
}

static esp_err_t read_reg(uint16_t reg, uint8_t *val) {
  const uint8_t params[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
  uint8_t reply[1];
  size_t reply_len = 0;
  esp_err_t ret = command(CMD_READ_REGISTER, params, sizeof(params), reply,
                          sizeof(reply), &reply_len, LOCAL_TIMEOUT_MS);
  if (ret != ESP_OK)
    return ret;
  if (reply_len != 1)
    return ESP_ERR_INVALID_RESPONSE;
  *val = reply[0];
  return ESP_OK;
}

/* ---------- Bring-up ---------- */

static esp_err_t identify(void) {
  uint8_t version[4];
  size_t len = 0;
  /* The first command also wakes the chip, and may be lost doing it */
  esp_err_t ret = command(CMD_FIRMWARE_VERSION, NULL, 0, version,
                          sizeof(version), &len, LOCAL_TIMEOUT_MS);
  if (ret != ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(5));
    ret = command(CMD_FIRMWARE_VERSION, NULL, 0, version, sizeof(version),
                  &len, LOCAL_TIMEOUT_MS);
  }
  if (ret != ESP_OK)
    return ret;
  if (len != 4 || version[0] != IC_PN532)
    return ESP_ERR_NOT_FOUND;
  ESP_LOGI(TAG, "PN532 firmware %u.%u", version[1], version[2]);
  return ESP_OK;
}

static esp_err_t pn_init(i2c_master_bus_handle_t bus) {
  if (!bus)
    return ESP_ERR_INVALID_ARG;
  if (ready)
    return ESP_OK;

  if (!dev) {
    /* Address-only probe first: nothing is written to 0x24 unless something
       there answers, and only a PN532 survives identify() below. */
    if (i2c_master_probe(bus, I2C_ADDR, I2C_TIMEOUT_MS) != ESP_OK)
      return ESP_ERR_NOT_FOUND;

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (ret != ESP_OK) {
      dev = NULL;
      return ret;
    }
  }

  static const uint8_t sam_normal[] = {0x01, 0x00, 0x00}; /* no SAM, no IRQ */
  static const uint8_t no_auto[] = {0x00}; /* no automatic RATS or ATR */
  static const uint8_t deadline[] = {0x02, 0x00, 0x0B, TIMEOUT_CODE};

  if (identify() != ESP_OK) {
    ESP_LOGW(TAG, "No PN532 at 0x%02x", I2C_ADDR);
    pn_deinit();
    return ESP_ERR_NOT_FOUND;
  }

  /* The tag layer selects cards itself: nothing automatic from the
     firmware. Then the CIU as an ISO14443A initiator at 106 kbit/s with raw
     frames — no CRC added or checked by the chip, parity left to it. */
  static const reg_val_t raw_mode[] = {
      {CIU_CONTROL, 0x10}, /* initiator */
      {CIU_STATUS2, 0x00}, /* crypto1 left over from an earlier session */
      {CIU_TX_MODE, 0x00},     {CIU_RX_MODE, 0x00},
      {CIU_TX_AUTO, 0x40}, /* force 100% ASK */
      {CIU_MANUAL_RCV, 0x00},  {CIU_BIT_FRAMING, 0x00},
  };
  ready = true;
  if (command(CMD_SAM_CONFIGURATION, sam_normal, sizeof(sam_normal), NULL, 0,
              NULL, LOCAL_TIMEOUT_MS) != ESP_OK ||
      command(CMD_SET_PARAMETERS, no_auto, sizeof(no_auto), NULL, 0, NULL,
              LOCAL_TIMEOUT_MS) != ESP_OK ||
      pn_antenna(false) != ESP_OK ||
      write_regs(raw_mode, sizeof(raw_mode) / sizeof(raw_mode[0])) != ESP_OK ||
      command(CMD_RF_CONFIGURATION, deadline, sizeof(deadline), NULL, 0, NULL,
              LOCAL_TIMEOUT_MS) != ESP_OK) {
    pn_deinit();
    return ESP_FAIL;
  }

  tx_bits_cached = 0;
  crypto_on = false;
  return ESP_OK;
}

static void pn_deinit(void) {
  if (dev) {
    if (ready)
      (void)pn_antenna(false);
    (void)i2c_master_bus_rm_device(dev);
    dev = NULL;
  }
  ready = false;
  crypto_on = false;
  tx_bits_cached = -1;
}

static bool pn_is_ready(void) { return ready; }

static esp_err_t pn_antenna(bool on) {
  if (!dev)
    return ESP_ERR_INVALID_STATE;
  const uint8_t params[2] = {0x01, on ? 0x01 : 0x00};
  esp_err_t ret = command(CMD_RF_CONFIGURATION, params, sizeof(params), NULL,
                          0, NULL, LOCAL_TIMEOUT_MS);
  if (!on)
    crypto_on = false;
  return ret;
}

/* ---------- Frame exchange ---------- */

static esp_err_t pn_transceive(const uint8_t *send, size_t send_len,
                               uint8_t tx_last_bits, uint8_t *recv,
                               size_t *recv_len, uint8_t *rx_last_bits) {
  if (!ready || !send || send_len == 0 || send_len > PCD_FIFO_SIZE)
    return ESP_ERR_INVALID_ARG;
  if (tx_last_bits > 7)
    return ESP_ERR_INVALID_ARG;

  if (tx_bits_cached != tx_last_bits) {
    tx_bits_cached = -1;
    if (write_reg(CIU_BIT_FRAMING, tx_last_bits) != ESP_OK)
      return ESP_FAIL;
    tx_bits_cached = tx_last_bits;
  }

  uint8_t reply[MAX_REPLY];
  size_t reply_len = 0;
  esp_err_t ret =
      command(CMD_COMMUNICATE_THRU, send, send_len, reply, sizeof(reply),
              &reply_len, TIMEOUT_CODE_MS + COMMAND_MARGIN_MS);
  if (ret != ESP_OK)
    goto out;

  ret = ESP_ERR_INVALID_RESPONSE;
  if (reply_len < 1)
    goto out;
  const uint8_t error = reply[0] & STATUS_ERR_MASK;
  const bool want_reply = recv && recv_len;
  if (error == STATUS_ERR_TIMEOUT) {
    /* Silence is the expected answer to HALT */
    ret = want_reply ? ESP_ERR_TIMEOUT : ESP_OK;
    goto out;
  }
  /* Parity, CRC, collision, framing: the frame is garbage. Collisions are
     reported, not resolved — Kern asks for a single card. */
  if (error)
    goto out;
  if (!want_reply) {
    ret = ESP_OK;
    goto out;
  }

  /* The tag chose this length; refuse rather than truncate */
  const size_t data_len = reply_len - 1;
  if (data_len > *recv_len) {
    ret = ESP_ERR_INVALID_SIZE;
    goto out;
  }
  memcpy(recv, &reply[1], data_len);
  *recv_len = data_len;

  if (rx_last_bits) {
    /* Valid bits of the last byte matter only for short frames without a
       CRC (ATQA, the 4-bit MIFARE ACK); for the rest skip a round trip. */
    uint8_t control = 0;
    if (data_len < 3) {
      if (read_reg(CIU_CONTROL, &control) != ESP_OK) {
        ret = ESP_FAIL;
        goto out;
      }
    }
    *rx_last_bits = control & 0x07;
  }
  ret = ESP_OK;

out:
  wipe_fn(reply, 0, sizeof(reply));
  return ret;
}

/* CRC_A per ISO 14443-3, low byte first */
static esp_err_t pn_calc_crc(const uint8_t *data, size_t len, uint8_t out[2]) {
  if (!ready || !data || !out || len == 0 || len > PCD_FIFO_SIZE)
    return ESP_ERR_INVALID_ARG;
  uint16_t crc = 0x6363;
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i] ^ (uint8_t)crc;
    b ^= (uint8_t)(b << 4);
    crc = (crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ (b >> 4);
  }
  out[0] = (uint8_t)crc;
  out[1] = (uint8_t)(crc >> 8);
  return ESP_OK;
}

/* ---------- MIFARE Classic authentication ---------- */

/*
 * Run the CIU's own MFAuthent through its registers, as on an MFRC522: the
 * firmware never has to know the tag, which was selected with raw frames.
 */
static esp_err_t pn_mf_authenticate(uint8_t cmd, uint8_t block,
                                    const uint8_t key[6], const uint8_t *uid,
                                    uint8_t uid_len) {
  if (!ready || !key || !uid)
    return ESP_ERR_INVALID_ARG;
  if (cmd != PICC_CMD_MF_AUTH_KEY_A && cmd != PICC_CMD_MF_AUTH_KEY_B)
    return ESP_ERR_INVALID_ARG;
  if (uid_len < 4)
    return ESP_ERR_INVALID_ARG;

  const uint8_t *uid_tail = uid + (uid_len - 4);
  uint8_t payload[12];
  payload[0] = cmd;
  payload[1] = block;
  memcpy(&payload[2], key, 6);
  memcpy(&payload[8], uid_tail, 4);

  reg_val_t regs[2 + sizeof(payload) + 1];
  size_t n = 0;
  regs[n++] = (reg_val_t){CIU_COMMAND, CIU_CMD_IDLE};
  regs[n++] = (reg_val_t){CIU_FIFO_LEVEL, 0x80}; /* flush FIFO */
  for (size_t i = 0; i < sizeof(payload); i++)
    regs[n++] = (reg_val_t){CIU_FIFO_DATA, payload[i]};
  regs[n++] = (reg_val_t){CIU_COMMAND, CIU_CMD_MF_AUTHENT};

  esp_err_t ret = write_regs(regs, n);
  wipe_fn(payload, 0, sizeof(payload));
  wipe_fn(regs, 0, sizeof(regs));
  if (ret != ESP_OK)
    return ret;

  /* Crypto1On in Status2 is the only reliable success signal */
  bool on = false;
  const int64_t deadline =
      esp_timer_get_time() + (int64_t)LOCAL_TIMEOUT_MS * 1000;
  do {
    uint8_t status2 = 0;
    if (read_reg(CIU_STATUS2, &status2) != ESP_OK)
      break;
    if (status2 & STATUS2_CRYPTO1_ON) {
      on = true;
      break;
    }
  } while (esp_timer_get_time() < deadline);

  crypto_on = on;
  (void)write_reg(CIU_COMMAND, CIU_CMD_IDLE);
  return on ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void pn_stop_crypto(void) {
  if (!ready)
    return;
  uint8_t status2 = 0;
  if (read_reg(CIU_STATUS2, &status2) == ESP_OK)
    (void)write_reg(CIU_STATUS2, status2 & (uint8_t)~STATUS2_CRYPTO1_ON);
  crypto_on = false;
}

static bool pn_crypto_active(void) { return crypto_on; }

const pcd_driver_t pcd_pn532_driver = {
    .name = "PN532",
    .init = pn_init,
    .deinit = pn_deinit,
    .is_ready = pn_is_ready,
    .antenna = pn_antenna,
    .transceive = pn_transceive,
    .calc_crc = pn_calc_crc,
    .mf_authenticate = pn_mf_authenticate,
    .stop_crypto = pn_stop_crypto,
    .crypto_active = pn_crypto_active,
};
