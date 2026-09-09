/*
 * ES8311 speaker output — see include/audio.h
 *
 * The register sequence below is Espressif's es8311 component bring-up
 * (esp-bsp/components/es8311) reduced to the one configuration Kern uses.
 * That driver carries a 90-row table of clock coefficients covering every
 * MCLK/sample-rate pair the codec supports; Kern needs a single row, so the
 * dividers here are constants with the row they came from written next to
 * them. Same writes on the wire, none of the search.
 *
 * Left out on purpose: the ADC serial port (0x0A), the analog microphone
 * enable (0x14) and the ADC gain (0x17). Not calling them is what keeps the
 * on-board microphones unpowered.
 */

#include "audio.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO";

/* ---------- Codec registers ---------- */

#define REG_RESET 0x00
#define REG_CLK_MANAGER 0x01
#define REG_CLK_DIV 0x02
#define REG_ADC_OSR 0x03
#define REG_DAC_OSR 0x04
#define REG_CLK_ADC_DAC 0x05
#define REG_BCLK_DIV 0x06
#define REG_LRCK_H 0x07
#define REG_LRCK_L 0x08
#define REG_SDP_IN 0x09
#define REG_SYSTEM_0D 0x0D
#define REG_SYSTEM_0E 0x0E
#define REG_SYSTEM_12 0x12
#define REG_SYSTEM_13 0x13
#define REG_ADC_1C 0x1C
#define REG_DAC_MUTE 0x31
#define REG_DAC_VOLUME 0x32
#define REG_DAC_RAMP 0x37
#define REG_CHIP_ID1 0xFD
#define REG_CHIP_ID2 0xFE

/* Coefficient row for MCLK 4.096 MHz / 16 kHz, lifted from the reference
 * table: pre_div 1, pre_multi 1x, adc_div 1, dac_div 1, single speed,
 * lrck 0x00ff, bclk_div 4, osr 0x10. Everything below is that row already
 * folded into the register layout. */
#define CLK_ADC_OSR 0x10
#define CLK_DAC_OSR 0x10
#define CLK_ADC_DAC_DIV 0x00
#define CLK_BCLK_DIV 0x03 /* the register wants bclk_div - 1 */
#define CLK_LRCK_H 0x00
#define CLK_LRCK_L 0xFF

#define SDP_16_BIT 0x0C /* 16-bit samples, I2S format */
#define DAC_MUTE_BITS 0x60

#define I2C_TIMEOUT_MS 100

/* Silence fed around the amplifier's transitions. The NS4150B clicks if it is
 * switched while the DAC output is at an arbitrary level; a few milliseconds
 * of zeros on either side costs nothing and keeps it quiet. */
#define PA_SETTLE_SAMPLES (AUDIO_SAMPLE_RATE / 100) /* 10 ms */

/* Mono samples converted to interleaved stereo per I2S write. */
#define FRAME_SAMPLES 128

static i2c_master_dev_handle_t codec = NULL;
static i2s_chan_handle_t tx_chan = NULL;
static bool amp_on = false;

/* ---------- I2C ---------- */

static esp_err_t reg_write(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(codec, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val) {
  return i2c_master_transmit_receive(codec, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

/* Read-modify-write, for the registers whose untouched bits matter. */
static esp_err_t reg_update(uint8_t reg, uint8_t clear, uint8_t set) {
  uint8_t val;
  esp_err_t ret = reg_read(reg, &val);
  if (ret != ESP_OK)
    return ret;
  return reg_write(reg, (uint8_t)((val & (uint8_t)~clear) | set));
}

/* ---------- Bring-up ---------- */

static esp_err_t codec_configure(void) {
  /* Reset, then power on. */
  ESP_RETURN_ON_ERROR(reg_write(REG_RESET, 0x1F), TAG, "reset");
  vTaskDelay(pdMS_TO_TICKS(20));
  ESP_RETURN_ON_ERROR(reg_write(REG_RESET, 0x00), TAG, "reset clear");
  ESP_RETURN_ON_ERROR(reg_write(REG_RESET, 0x80), TAG, "power on");

  /* All clocks enabled, MCLK taken from the MCLK pin, neither MCLK nor SCLK
   * inverted. */
  ESP_RETURN_ON_ERROR(reg_write(REG_CLK_MANAGER, 0x3F), TAG, "clk manager");
  ESP_RETURN_ON_ERROR(reg_update(REG_BCLK_DIV, 0x20, 0x00), TAG, "sclk pol");

  /* Dividers. 0x02 keeps its low three bits: pre_div and pre_multi are both
   * unity for this row, so there is nothing to set. */
  ESP_RETURN_ON_ERROR(reg_update(REG_CLK_DIV, 0xF8, 0x00), TAG, "clk div");
  ESP_RETURN_ON_ERROR(reg_write(REG_ADC_OSR, CLK_ADC_OSR), TAG, "adc osr");
  ESP_RETURN_ON_ERROR(reg_write(REG_DAC_OSR, CLK_DAC_OSR), TAG, "dac osr");
  ESP_RETURN_ON_ERROR(reg_write(REG_CLK_ADC_DAC, CLK_ADC_DAC_DIV), TAG, "div");
  ESP_RETURN_ON_ERROR(reg_update(REG_BCLK_DIV, 0x1F, CLK_BCLK_DIV), TAG,
                      "bclk div");
  ESP_RETURN_ON_ERROR(reg_update(REG_LRCK_H, 0x3F, CLK_LRCK_H), TAG, "lrck h");
  ESP_RETURN_ON_ERROR(reg_write(REG_LRCK_L, CLK_LRCK_L), TAG, "lrck l");

  /* Codec is the I2S slave; the P4 drives the clocks. */
  ESP_RETURN_ON_ERROR(reg_update(REG_RESET, 0x40, 0x00), TAG, "slave mode");
  ESP_RETURN_ON_ERROR(reg_write(REG_SDP_IN, SDP_16_BIT), TAG, "sdp in");

  /* Power up the analog side and the DAC, route it to the output driver. */
  ESP_RETURN_ON_ERROR(reg_write(REG_SYSTEM_0D, 0x01), TAG, "analog on");
  ESP_RETURN_ON_ERROR(reg_write(REG_SYSTEM_0E, 0x02), TAG, "pga on");
  ESP_RETURN_ON_ERROR(reg_write(REG_SYSTEM_12, 0x00), TAG, "dac on");
  ESP_RETURN_ON_ERROR(reg_write(REG_SYSTEM_13, 0x10), TAG, "output on");
  ESP_RETURN_ON_ERROR(reg_write(REG_ADC_1C, 0x6A), TAG, "adc dc offset");
  ESP_RETURN_ON_ERROR(reg_write(REG_DAC_RAMP, 0x08), TAG, "dac eq bypass");

  ESP_RETURN_ON_ERROR(reg_write(REG_DAC_MUTE, 0x00), TAG, "unmute");
  /* Kconfig volume is 0-100; the register is 0-255 with 0 as true silence. */
  const uint8_t vol =
      CONFIG_KERN_AUDIO_VOLUME == 0
          ? 0
          : (uint8_t)((CONFIG_KERN_AUDIO_VOLUME * 256 / 100) - 1);
  return reg_write(REG_DAC_VOLUME, vol);
}

static esp_err_t i2s_open(void) {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_chan, NULL), TAG,
                      "no free I2S channel");

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = CONFIG_KERN_AUDIO_MCLK_GPIO,
              .bclk = CONFIG_KERN_AUDIO_BCLK_GPIO,
              .ws = CONFIG_KERN_AUDIO_LRCK_GPIO,
              .dout = CONFIG_KERN_AUDIO_DOUT_GPIO,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {0},
          },
  };
  /* The codec runs off MCLK at 256x the sample rate, which is what the
   * divider row above was picked for. */
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  esp_err_t ret = i2s_channel_init_std_mode(tx_chan, &std_cfg);
  if (ret != ESP_OK)
    return ret;
  return i2s_channel_enable(tx_chan);
}

/* ---------- Amplifier ---------- */

static void write_silence(size_t samples) {
  static const int16_t zeros[FRAME_SAMPLES * 2] = {0};
  size_t written;
  while (samples > 0) {
    const size_t chunk = samples < FRAME_SAMPLES ? samples : FRAME_SAMPLES;
    (void)i2s_channel_write(tx_chan, zeros, chunk * 2 * sizeof(int16_t),
                            &written, portMAX_DELAY);
    samples -= chunk;
  }
}

/* Raise the amplifier behind a run of zeros, so it settles on silence rather
 * than on whatever the DAC happened to be holding. */
static void amp_raise(void) {
  if (amp_on)
    return;
  write_silence(PA_SETTLE_SAMPLES);
  gpio_set_level(CONFIG_KERN_AUDIO_PA_GPIO, 1);
  amp_on = true;
}

static void amp_lower(void) {
  if (!amp_on)
    return;
  write_silence(PA_SETTLE_SAMPLES);
  gpio_set_level(CONFIG_KERN_AUDIO_PA_GPIO, 0);
  amp_on = false;
}

/* ---------- Public API ---------- */

esp_err_t audio_init(i2c_master_bus_handle_t bus) {
  if (codec)
    return ESP_OK;
  if (!bus)
    return ESP_ERR_INVALID_ARG;

  /* A board without the speaker circuit is a board where nothing answers. */
  if (i2c_master_probe(bus, CONFIG_KERN_AUDIO_I2C_ADDR, I2C_TIMEOUT_MS) !=
      ESP_OK) {
    ESP_LOGW(TAG, "No codec at 0x%02X", CONFIG_KERN_AUDIO_I2C_ADDR);
    return ESP_ERR_NOT_FOUND;
  }

  const i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = CONFIG_KERN_AUDIO_I2C_ADDR,
      .scl_speed_hz = 100000,
  };
  ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &codec), TAG,
                      "bus add");

  /* Identity is logged, not enforced: the probe already proved something is
   * there, and refusing to play over an unexpected revision byte would be a
   * worse failure than a wrong-sounding beep. */
  uint8_t id1 = 0, id2 = 0;
  if (reg_read(REG_CHIP_ID1, &id1) == ESP_OK &&
      reg_read(REG_CHIP_ID2, &id2) == ESP_OK)
    ESP_LOGI(TAG, "Codec ID %02X%02X", id1, id2);

  const gpio_config_t pa_cfg = {
      .pin_bit_mask = 1ULL << CONFIG_KERN_AUDIO_PA_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t ret = gpio_config(&pa_cfg);
  if (ret == ESP_OK)
    ret = gpio_set_level(CONFIG_KERN_AUDIO_PA_GPIO, 0);
  if (ret == ESP_OK)
    ret = i2s_open();
  if (ret == ESP_OK)
    ret = codec_configure();

  if (ret != ESP_OK) {
    audio_deinit();
    return ret;
  }
  return ESP_OK;
}

esp_err_t audio_write(const int16_t *pcm, size_t samples) {
  if (!tx_chan || !codec)
    return ESP_ERR_INVALID_STATE;
  if (!pcm && samples)
    return ESP_ERR_INVALID_ARG;

  amp_raise();

  /* The DAC takes one channel, but the codec is clocked for two slots and the
   * bring-up this driver follows feeds stereo frames. Duplicating in software
   * costs 32 KB/s of DMA and removes any question about how a mono slot is
   * mapped onto the wire. */
  int16_t frame[FRAME_SAMPLES * 2];
  while (samples > 0) {
    const size_t chunk = samples < FRAME_SAMPLES ? samples : FRAME_SAMPLES;
    for (size_t i = 0; i < chunk; i++) {
      frame[i * 2] = pcm[i];
      frame[i * 2 + 1] = pcm[i];
    }
    size_t written;
    ESP_RETURN_ON_ERROR(i2s_channel_write(tx_chan, frame,
                                          chunk * 2 * sizeof(int16_t), &written,
                                          portMAX_DELAY),
                        TAG, "i2s write");
    pcm += chunk;
    samples -= chunk;
  }
  return ESP_OK;
}

void audio_stop(void) {
  if (!tx_chan)
    return;
  /* Drop what the DMA still holds first — an interrupted announcement should
   * stop now, not after the buffered tail of it has played. Only then walk the
   * amplifier down through silence. */
  (void)i2s_channel_disable(tx_chan);
  (void)i2s_channel_enable(tx_chan);
  amp_lower();
}

void audio_deinit(void) {
  if (tx_chan) {
    if (amp_on) {
      gpio_set_level(CONFIG_KERN_AUDIO_PA_GPIO, 0);
      amp_on = false;
    }
    (void)i2s_channel_disable(tx_chan);
    (void)i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }
  if (codec) {
    (void)reg_write(REG_DAC_MUTE, DAC_MUTE_BITS);
    (void)i2c_master_bus_rm_device(codec);
    codec = NULL;
  }
}
