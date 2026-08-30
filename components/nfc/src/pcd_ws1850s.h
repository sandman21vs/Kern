/*
 * WS1850S reader (PCD) — register access and ISO14443A framing
 *
 * The WS1850S is register-compatible with the NXP MFRC522 but is not the same
 * part: notably it does not report the MFRC522's VersionReg values, so
 * presence is probed by writing a register and reading it back rather than by
 * matching a version byte.
 *
 * Everything here is bounded. Frame length, wait loops and FIFO reads all have
 * hard limits, because the far side of the antenna is a device someone else
 * built.
 */

#ifndef PCD_WS1850S_H
#define PCD_WS1850S_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/* The reader's FIFO is 64 bytes; no frame can exceed it. */
#define PCD_FIFO_SIZE 64

/* A CRC_A is two bytes. Receive buffers must have room for it even when the
 * caller only wants the payload: the CRC arrives with the frame and is
 * verified before being stripped. */
#define PCD_CRC_LEN 2

/* PICC commands used by the tag layer. */
#define PICC_CMD_REQA 0x26
#define PICC_CMD_WUPA 0x52
#define PICC_CMD_HALT 0x50
#define PICC_CMD_SEL_CL1 0x93
#define PICC_CMD_SEL_CL2 0x95
#define PICC_CMD_MF_AUTH_KEY_A 0x60
#define PICC_CMD_MF_AUTH_KEY_B 0x61
#define PICC_CMD_MF_READ 0x30
#define PICC_CMD_MF_WRITE 0xA0
#define PICC_CMD_UL_WRITE 0xA2

/** Attach to an initialized I2C bus and bring the reader up with the field
 *  off. Idempotent. ESP_ERR_NOT_FOUND when nothing answers. */
esp_err_t pcd_init(i2c_master_bus_handle_t bus);

void pcd_deinit(void);

bool pcd_is_ready(void);

/** Energize or drop the antenna. */
esp_err_t pcd_antenna(bool on);

/**
 * Exchange one frame with the tag.
 *
 * @param send          bytes to transmit (at most PCD_FIFO_SIZE)
 * @param tx_last_bits  bits of the final byte to send; 0 sends all 8
 * @param recv          receive buffer, may be NULL when no reply is expected
 * @param recv_len      in: capacity of recv. out: bytes actually received.
 *                      A reply larger than the buffer is rejected outright —
 *                      truncating it would let the tag desynchronize us.
 * @param rx_last_bits  optional, receives valid bits of the final byte
 *
 * @return ESP_ERR_TIMEOUT when the tag says nothing, ESP_ERR_INVALID_SIZE
 *         when the reply does not fit, ESP_ERR_INVALID_CRC on a bad frame,
 *         ESP_ERR_INVALID_RESPONSE on a collision or protocol error.
 */
esp_err_t pcd_transceive(const uint8_t *send, size_t send_len,
                         uint8_t tx_last_bits, uint8_t *recv, size_t *recv_len,
                         uint8_t *rx_last_bits);

/** Same as pcd_transceive, but appends a CRC_A to the frame and verifies the
 *  CRC_A on the reply, stripping it from *recv_len.
 *
 *  recv must be large enough for the payload *plus* PCD_CRC_LEN — the CRC
 *  arrives as part of the frame, and an undersized buffer is rejected as an
 *  oversized reply. */
esp_err_t pcd_transceive_crc(const uint8_t *send, size_t send_len,
                             uint8_t *recv, size_t *recv_len);

/** Compute a CRC_A using the reader's own CRC coprocessor. */
esp_err_t pcd_calc_crc(const uint8_t *data, size_t len, uint8_t out[2]);

/**
 * Run MIFARE Classic authentication for one sector. The crypto1 state lives
 * in the reader; every authenticated exchange afterwards must be followed by
 * pcd_stop_crypto() before the tag can be released.
 *
 * @param cmd  PICC_CMD_MF_AUTH_KEY_A or PICC_CMD_MF_AUTH_KEY_B
 * @param uid  tag UID; the last four bytes are the ones used
 */
esp_err_t pcd_mf_authenticate(uint8_t cmd, uint8_t block, const uint8_t key[6],
                              const uint8_t *uid, uint8_t uid_len);

/** Clear the crypto1 state left by pcd_mf_authenticate(). */
void pcd_stop_crypto(void);

/** True while the reader holds an authenticated crypto1 session. */
bool pcd_crypto_active(void);

#endif /* PCD_WS1850S_H */
