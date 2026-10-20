// Reader dispatch — picks the module on the bus (see pcd.h)

#include "pcd.h"

#include <string.h>

#include "esp_log.h"

#if !CONFIG_KERN_NFC_WS1850S && !CONFIG_KERN_NFC_PN532
#error "CONFIG_KERN_NFC needs at least one reader backend"
#endif

static const char *TAG = "NFC_PCD";

/* Probe order. The WS1850S goes first so a board that already worked with
 * the RFID Unit 2 talks to exactly the same address it always did. */
static const pcd_driver_t *const drivers[] = {
#if CONFIG_KERN_NFC_WS1850S
    &pcd_ws1850s_driver,
#endif
#if CONFIG_KERN_NFC_PN532
    &pcd_pn532_driver,
#endif
};

static const pcd_driver_t *active = NULL;

esp_err_t pcd_init(i2c_master_bus_handle_t bus) {
  if (!bus)
    return ESP_ERR_INVALID_ARG;
  if (active && active->is_ready())
    return ESP_OK;

  active = NULL;
  for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
    if (drivers[i]->init(bus) == ESP_OK) {
      active = drivers[i];
      ESP_LOGI(TAG, "Reader: %s", active->name);
      return ESP_OK;
    }
  }
  return ESP_ERR_NOT_FOUND;
}

void pcd_deinit(void) {
  if (active)
    active->deinit();
  active = NULL;
}

bool pcd_is_ready(void) { return active && active->is_ready(); }

const char *pcd_name(void) { return pcd_is_ready() ? active->name : NULL; }

esp_err_t pcd_antenna(bool on) {
  if (!active)
    return ESP_ERR_INVALID_STATE;
  return active->antenna(on);
}

esp_err_t pcd_transceive(const uint8_t *send, size_t send_len,
                         uint8_t tx_last_bits, uint8_t *recv, size_t *recv_len,
                         uint8_t *rx_last_bits) {
  if (!active)
    return ESP_ERR_INVALID_STATE;
  return active->transceive(send, send_len, tx_last_bits, recv, recv_len,
                            rx_last_bits);
}

esp_err_t pcd_calc_crc(const uint8_t *data, size_t len, uint8_t out[2]) {
  if (!active)
    return ESP_ERR_INVALID_STATE;
  return active->calc_crc(data, len, out);
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

esp_err_t pcd_mf_authenticate(uint8_t cmd, uint8_t block, const uint8_t key[6],
                              const uint8_t *uid, uint8_t uid_len) {
  if (!active)
    return ESP_ERR_INVALID_STATE;
  return active->mf_authenticate(cmd, block, key, uid, uid_len);
}

void pcd_stop_crypto(void) {
  if (active)
    active->stop_crypto();
}

bool pcd_crypto_active(void) { return active && active->crypto_active(); }
