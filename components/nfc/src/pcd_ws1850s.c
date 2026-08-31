// WS1850S reader — register access and ISO14443A framing (see pcd_ws1850s.h)

#include "pcd_ws1850s.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "NFC_PCD";

/* Register map (MFRC522-compatible) */
#define REG_COMMAND 0x01
#define REG_COM_IRQ 0x04
#define REG_DIV_IRQ 0x05
#define REG_ERROR 0x06
#define REG_FIFO_DATA 0x09
#define REG_FIFO_LEVEL 0x0A
#define REG_CONTROL 0x0C
#define REG_BIT_FRAMING 0x0D
#define REG_COLL 0x0E
#define REG_MODE 0x11
#define REG_TX_CONTROL 0x14
#define REG_TX_ASK 0x15
#define REG_CRC_RESULT_H 0x21
#define REG_CRC_RESULT_L 0x22
#define REG_STATUS2 0x08
#define REG_T_MODE 0x2A
#define REG_T_PRESCALER 0x2B
#define REG_T_RELOAD_H 0x2C
#define REG_T_RELOAD_L 0x2D

/* Reader commands */
#define CMD_IDLE 0x00
#define CMD_CALC_CRC 0x03
#define CMD_TRANSCEIVE 0x0C
#define CMD_MF_AUTHENT 0x0E
#define CMD_SOFT_RESET 0x0F

/* ErrorReg bits. Any of the fatal ones means the frame is garbage; CRCErr is
 * excluded because framing without a CRC legitimately leaves it set. */
#define ERR_PROTOCOL 0x01
#define ERR_PARITY 0x02
#define ERR_COLL 0x08
#define ERR_BUFFER_OVFL 0x10
#define ERR_FATAL_MASK (ERR_BUFFER_OVFL | ERR_COLL | ERR_PARITY | ERR_PROTOCOL)

#define IRQ_TIMER 0x01
#define IRQ_RX 0x20
#define IRQ_IDLE 0x10

/* An exchange is bounded twice over: the reader's own 25 ms timer, and this
 * wall-clock deadline in case the reader itself stops answering. */
#define EXCHANGE_TIMEOUT_US 60000
#define CRC_TIMEOUT_US 20000

#define I2C_TIMEOUT_MS CONFIG_KERN_NFC_I2C_TIMEOUT_MS

static i2c_master_dev_handle_t dev = NULL;
static bool ready = false;
static bool crypto_on = false;

/* ---------- Register access ---------- */

static esp_err_t reg_write(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val) {
  return i2c_master_transmit_receive(dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

/* Reading FIFODataReg repeatedly drains the FIFO, so one burst read works. */
static esp_err_t reg_read_burst(uint8_t reg, uint8_t *buf, size_t len) {
  return i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t reg_set_bits(uint8_t reg, uint8_t mask) {
  uint8_t val = 0;
  esp_err_t ret = reg_read(reg, &val);
  if (ret != ESP_OK)
    return ret;
  return reg_write(reg, val | mask);
}

static esp_err_t reg_clear_bits(uint8_t reg, uint8_t mask) {
  uint8_t val = 0;
  esp_err_t ret = reg_read(reg, &val);
  if (ret != ESP_OK)
    return ret;
  return reg_write(reg, val & (uint8_t)~mask);
}

/* ---------- Bring-up ---------- */

/*
 * Presence probe. VersionReg is useless here — the WS1850S does not return the
 * MFRC522's 0x91/0x92 — so instead write two patterns to a harmless register
 * and read them back. A missing or dead module fails the I2C transfer or
 * returns something else.
 */
static esp_err_t probe(void) {
  static const uint8_t patterns[] = {0x55, 0xAA};
  for (size_t i = 0; i < sizeof(patterns); i++) {
    uint8_t back = 0;
    if (reg_write(REG_T_RELOAD_L, patterns[i]) != ESP_OK)
      return ESP_ERR_NOT_FOUND;
    if (reg_read(REG_T_RELOAD_L, &back) != ESP_OK)
      return ESP_ERR_NOT_FOUND;
    if (back != patterns[i])
      return ESP_ERR_NOT_FOUND;
  }
  return ESP_OK;
}

static esp_err_t soft_reset(void) {
  esp_err_t ret = reg_write(REG_COMMAND, CMD_SOFT_RESET);
  if (ret != ESP_OK)
    return ret;

  /* The datasheet allows the reset to take a moment; poll rather than
     guessing a delay, but give up instead of spinning. */
  for (int i = 0; i < 10; i++) {
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t cmd = 0;
    if (reg_read(REG_COMMAND, &cmd) == ESP_OK && !(cmd & 0x10))
      return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}

esp_err_t pcd_init(i2c_master_bus_handle_t bus) {
  if (!bus)
    return ESP_ERR_INVALID_ARG;
  if (ready)
    return ESP_OK;

  if (!dev) {
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_KERN_NFC_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (ret != ESP_OK) {
      dev = NULL;
      return ret;
    }
  }

  esp_err_t ret = probe();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "No reader at 0x%02x", CONFIG_KERN_NFC_I2C_ADDR);
    pcd_deinit();
    return ESP_ERR_NOT_FOUND;
  }

  ret = soft_reset();
  if (ret != ESP_OK) {
    pcd_deinit();
    return ret;
  }

  /* Timer: TAuto, prescaler 0xA9 -> 40 kHz, reload 1000 -> 25 ms per
     exchange. This is what stops a silent tag from hanging a read. */
  if (reg_write(REG_T_MODE, 0x80) != ESP_OK ||
      reg_write(REG_T_PRESCALER, 0xA9) != ESP_OK ||
      reg_write(REG_T_RELOAD_H, 0x03) != ESP_OK ||
      reg_write(REG_T_RELOAD_L, 0xE8) != ESP_OK ||
      reg_write(REG_TX_ASK, 0x40) != ESP_OK || /* force 100% ASK */
      reg_write(REG_MODE, 0x3D) != ESP_OK) {   /* CRC preset 0x6363 */
    pcd_deinit();
    return ESP_FAIL;
  }

  /* Come up with the antenna dark; callers energize it deliberately. */
  if (pcd_antenna(false) != ESP_OK) {
    pcd_deinit();
    return ESP_FAIL;
  }

  crypto_on = false;
  ready = true;
  return ESP_OK;
}

void pcd_deinit(void) {
  if (dev) {
    if (ready)
      (void)pcd_antenna(false);
    (void)i2c_master_bus_rm_device(dev);
    dev = NULL;
  }
  ready = false;
  crypto_on = false;
}

bool pcd_is_ready(void) { return ready; }

esp_err_t pcd_antenna(bool on) {
  if (!dev)
    return ESP_ERR_INVALID_STATE;
  return on ? reg_set_bits(REG_TX_CONTROL, 0x03)
            : reg_clear_bits(REG_TX_CONTROL, 0x03);
}

/* ---------- Frame exchange ---------- */

static esp_err_t wait_irq(uint8_t wait_mask, int64_t timeout_us) {
  const int64_t deadline = esp_timer_get_time() + timeout_us;

  do {
    uint8_t irq = 0;
    esp_err_t ret = reg_read(REG_COM_IRQ, &irq);
    if (ret != ESP_OK)
      return ret;
    if (irq & wait_mask)
      return ESP_OK;
    if (irq & IRQ_TIMER)
      return ESP_ERR_TIMEOUT;
  } while (esp_timer_get_time() < deadline);

  return ESP_ERR_TIMEOUT;
}

/* Drain the reply the tag left in the FIFO, refusing anything oversized. */
static esp_err_t collect_reply(uint8_t *recv, size_t *recv_len,
                               uint8_t *rx_last_bits) {
  uint8_t level = 0;
  esp_err_t ret = reg_read(REG_FIFO_LEVEL, &level);
  if (ret != ESP_OK)
    return ret;

  /* The tag decides this number. Copying it into a smaller buffer is the
     classic MFRC522 overflow, so refuse rather than truncate: a short read
     would also leave the protocol out of step. */
  if (level > PCD_FIFO_SIZE)
    return ESP_ERR_INVALID_RESPONSE;
  if (!recv || level > *recv_len)
    return ESP_ERR_INVALID_SIZE;

  if (level > 0) {
    ret = reg_read_burst(REG_FIFO_DATA, recv, level);
    if (ret != ESP_OK)
      return ret;
  }
  *recv_len = level;

  uint8_t control = 0;
  ret = reg_read(REG_CONTROL, &control);
  if (ret != ESP_OK)
    return ret;
  /* RxLastBits is three bits wide; mask before it becomes shift arithmetic. */
  if (rx_last_bits)
    *rx_last_bits = control & 0x07;

  return ESP_OK;
}

esp_err_t pcd_transceive(const uint8_t *send, size_t send_len,
                         uint8_t tx_last_bits, uint8_t *recv, size_t *recv_len,
                         uint8_t *rx_last_bits) {
  if (!ready || !send || send_len == 0 || send_len > PCD_FIFO_SIZE)
    return ESP_ERR_INVALID_ARG;
  if (tx_last_bits > 7)
    return ESP_ERR_INVALID_ARG;

  if (reg_write(REG_COMMAND, CMD_IDLE) != ESP_OK ||
      reg_write(REG_COM_IRQ, 0x7F) != ESP_OK ||  /* clear IRQs */
      reg_write(REG_FIFO_LEVEL, 0x80) != ESP_OK) /* flush FIFO */
    return ESP_FAIL;

  /* Payload has to follow the register byte in one transaction. */
  uint8_t frame[1 + PCD_FIFO_SIZE];
  frame[0] = REG_FIFO_DATA;
  memcpy(&frame[1], send, send_len);
  if (i2c_master_transmit(dev, frame, send_len + 1, I2C_TIMEOUT_MS) != ESP_OK)
    return ESP_FAIL;

  if (reg_write(REG_BIT_FRAMING, tx_last_bits) != ESP_OK ||
      reg_write(REG_COMMAND, CMD_TRANSCEIVE) != ESP_OK ||
      reg_set_bits(REG_BIT_FRAMING, 0x80) != ESP_OK) /* StartSend */
    return ESP_FAIL;

  esp_err_t ret = wait_irq(IRQ_RX | IRQ_IDLE, EXCHANGE_TIMEOUT_US);
  (void)reg_clear_bits(REG_BIT_FRAMING, 0x80);
  if (ret != ESP_OK)
    return ret;

  uint8_t error = 0;
  if (reg_read(REG_ERROR, &error) != ESP_OK)
    return ESP_FAIL;
  if (error & ERR_FATAL_MASK) {
    /* Collisions are expected only with more than one card in the field;
       Kern treats that as "present a single card" rather than resolving it. */
    return ESP_ERR_INVALID_RESPONSE;
  }

  if (!recv || !recv_len) {
    (void)reg_write(REG_COMMAND, CMD_IDLE);
    return ESP_OK;
  }
  return collect_reply(recv, recv_len, rx_last_bits);
}

esp_err_t pcd_transceive_crc(const uint8_t *send, size_t send_len,
                             uint8_t *recv, size_t *recv_len) {
  if (!send || send_len == 0 || send_len + 2 > PCD_FIFO_SIZE)
    return ESP_ERR_INVALID_ARG;
  if (!recv || !recv_len || *recv_len < 2)
    return ESP_ERR_INVALID_ARG;

  uint8_t frame[PCD_FIFO_SIZE];
  memcpy(frame, send, send_len);
  esp_err_t ret = pcd_calc_crc(send, send_len, &frame[send_len]);
  if (ret != ESP_OK)
    return ret;

  size_t len = *recv_len;
  ret = pcd_transceive(frame, send_len + 2, 0, recv, &len, NULL);
  if (ret != ESP_OK)
    return ret;

  /* A reply carrying a CRC_A is at least three bytes; anything shorter is
     malformed regardless of what it claims to be. */
  if (len < 3)
    return ESP_ERR_INVALID_RESPONSE;

  uint8_t crc[2];
  ret = pcd_calc_crc(recv, len - 2, crc);
  if (ret != ESP_OK)
    return ret;
  if (crc[0] != recv[len - 2] || crc[1] != recv[len - 1])
    return ESP_ERR_INVALID_CRC;

  *recv_len = len - 2;
  return ESP_OK;
}

esp_err_t pcd_calc_crc(const uint8_t *data, size_t len, uint8_t out[2]) {
  if (!ready || !data || !out || len == 0 || len > PCD_FIFO_SIZE)
    return ESP_ERR_INVALID_ARG;

  if (reg_write(REG_COMMAND, CMD_IDLE) != ESP_OK ||
      reg_write(REG_DIV_IRQ, 0x04) != ESP_OK || /* clear CRCIRq */
      reg_write(REG_FIFO_LEVEL, 0x80) != ESP_OK)
    return ESP_FAIL;

  uint8_t frame[1 + PCD_FIFO_SIZE];
  frame[0] = REG_FIFO_DATA;
  memcpy(&frame[1], data, len);
  if (i2c_master_transmit(dev, frame, len + 1, I2C_TIMEOUT_MS) != ESP_OK)
    return ESP_FAIL;

  if (reg_write(REG_COMMAND, CMD_CALC_CRC) != ESP_OK)
    return ESP_FAIL;

  const int64_t deadline = esp_timer_get_time() + CRC_TIMEOUT_US;
  do {
    uint8_t irq = 0;
    if (reg_read(REG_DIV_IRQ, &irq) != ESP_OK)
      return ESP_FAIL;
    if (irq & 0x04) { /* CRCIRq */
      (void)reg_write(REG_COMMAND, CMD_IDLE);
      if (reg_read(REG_CRC_RESULT_L, &out[0]) != ESP_OK ||
          reg_read(REG_CRC_RESULT_H, &out[1]) != ESP_OK)
        return ESP_FAIL;
      return ESP_OK;
    }
  } while (esp_timer_get_time() < deadline);

  (void)reg_write(REG_COMMAND, CMD_IDLE);
  return ESP_ERR_TIMEOUT;
}

/* ---------- MIFARE Classic authentication ---------- */

esp_err_t pcd_mf_authenticate(uint8_t cmd, uint8_t block, const uint8_t key[6],
                              const uint8_t *uid, uint8_t uid_len) {
  if (!ready || !key || !uid)
    return ESP_ERR_INVALID_ARG;
  if (cmd != PICC_CMD_MF_AUTH_KEY_A && cmd != PICC_CMD_MF_AUTH_KEY_B)
    return ESP_ERR_INVALID_ARG;
  if (uid_len < 4)
    return ESP_ERR_INVALID_ARG;

  /* Classic authentication keys on the last four UID bytes, which is the
     whole UID for single-size tags and the tail for double-size ones. */
  const uint8_t *uid_tail = uid + (uid_len - 4);

  uint8_t payload[12];
  payload[0] = cmd;
  payload[1] = block;
  memcpy(&payload[2], key, 6);
  memcpy(&payload[8], uid_tail, 4);

  if (reg_write(REG_COMMAND, CMD_IDLE) != ESP_OK ||
      reg_write(REG_COM_IRQ, 0x7F) != ESP_OK ||
      reg_write(REG_FIFO_LEVEL, 0x80) != ESP_OK)
    return ESP_FAIL;

  uint8_t frame[1 + sizeof(payload)];
  frame[0] = REG_FIFO_DATA;
  memcpy(&frame[1], payload, sizeof(payload));
  if (i2c_master_transmit(dev, frame, sizeof(frame), I2C_TIMEOUT_MS) != ESP_OK)
    return ESP_FAIL;

  if (reg_write(REG_COMMAND, CMD_MF_AUTHENT) != ESP_OK)
    return ESP_FAIL;

  esp_err_t ret = wait_irq(IRQ_IDLE, EXCHANGE_TIMEOUT_US);
  if (ret != ESP_OK) {
    (void)reg_write(REG_COMMAND, CMD_IDLE);
    return ret;
  }

  /* Crypto1On in Status2Reg is the only reliable success signal. */
  uint8_t status2 = 0;
  if (reg_read(REG_STATUS2, &status2) != ESP_OK)
    return ESP_FAIL;
  if (!(status2 & 0x08)) {
    crypto_on = false;
    return ESP_ERR_INVALID_RESPONSE;
  }

  crypto_on = true;
  return ESP_OK;
}

void pcd_stop_crypto(void) {
  if (!ready)
    return;
  (void)reg_clear_bits(REG_STATUS2, 0x08);
  crypto_on = false;
}

bool pcd_crypto_active(void) { return crypto_on; }
