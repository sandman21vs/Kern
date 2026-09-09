/*
 * Speaker output — ES8311 codec on the board's I2S bus
 *
 * Codec: ES8311 at I2C 0x18, driving an NS4150B amplifier into the board's
 * 2-pin speaker header. The I2C bus handle is passed in rather than opened
 * here, so the component carries no board dependency: every Kern BSP exposes
 * bsp_i2c_get_handle(). The I2S pins come from Kconfig, the way the SD card
 * component takes its SDMMC pins.
 *
 * Four verbs, which is all Kern does with a speaker: bring it up, push
 * samples at it, cut the sound, put it away. One format only — 16 kHz, 16-bit
 * mono — because that is what the spoken lexicon is baked at, and a signer has
 * no reason to play anything else.
 *
 * Playback only. The record path is deliberately left unconfigured: these
 * boards carry microphones, and nothing in Kern should be able to turn one on.
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The one sample rate the codec is configured for. Callers that generate
 * samples need it; nothing else is supported. */
#define AUDIO_SAMPLE_RATE 16000

/**
 * Bind the codec to an initialized I2C bus, program it for 16 kHz mono
 * playback and open the I2S channel. The amplifier stays off until the first
 * audio_write(). Idempotent.
 *
 * @return ESP_ERR_NOT_FOUND when nothing answers at the codec address, which
 *         is what a board without the speaker circuit looks like.
 */
esp_err_t audio_init(i2c_master_bus_handle_t bus);

/**
 * Push mono samples to the speaker, blocking until the DMA has taken them.
 * Raises the amplifier on the first call after silence.
 *
 * @param pcm     signed 16-bit mono samples at AUDIO_SAMPLE_RATE
 * @param samples number of samples, not bytes
 */
esp_err_t audio_write(const int16_t *pcm, size_t samples);

/** Drop what is queued, flush the tail and lower the amplifier. Safe to call
 *  when nothing is playing. */
void audio_stop(void);

/** Close the I2S channel and detach the codec. Safe to call without a
 *  preceding audio_init(). */
void audio_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */
