/**
 * Audio Simulator — plays what the ES8311 driver would play, through SDL
 *
 * Implements the contract in components/audio/include/audio.h so the screen
 * reader can be developed and demonstrated on a desktop, without a board and
 * without a speaker soldered to one. That real header is on the include path
 * rather than copied here: platform/sd_card_sim/include/sd_card.h is a copy
 * that has already drifted from the component it mirrors, and one of those is
 * enough.
 *
 * SDL owns the pacing. audio_write() queues and then waits for the queue to
 * drain to a short backlog, which is what the I2S DMA does to a caller on the
 * device: it keeps the speech task from running ahead of the sound, so
 * interrupting an announcement actually cuts it off.
 */

#include "audio.h"

#include <SDL2/SDL.h>

#include "esp_log.h"

static const char *TAG = "AUDIO_SIM";

/* Let at most this much sound sit ahead of the caller. Roughly the device's
 * DMA depth: long enough not to stutter, short enough that a cancelled
 * announcement stops being audible immediately. */
#define QUEUE_AHEAD_MS 120

static SDL_AudioDeviceID device = 0;

static size_t queue_ahead_bytes(void) {
  return (size_t)AUDIO_SAMPLE_RATE * QUEUE_AHEAD_MS / 1000 * sizeof(int16_t);
}

esp_err_t audio_init(i2c_master_bus_handle_t bus) {
  (void)bus; /* no codec to talk to */
  if (device)
    return ESP_OK;

  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    ESP_LOGW(TAG, "SDL audio unavailable: %s", SDL_GetError());
    return ESP_ERR_NOT_FOUND;
  }

  /* Mono, because that is what the driver's callers produce. The device path
   * duplicates to stereo only because the codec is clocked for two slots. */
  SDL_AudioSpec want = {
      .freq = AUDIO_SAMPLE_RATE,
      .format = AUDIO_S16SYS,
      .channels = 1,
      .samples = 512,
  };
  SDL_AudioSpec have;
  device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!device) {
    ESP_LOGW(TAG, "No audio device: %s", SDL_GetError());
    return ESP_ERR_NOT_FOUND;
  }

  SDL_PauseAudioDevice(device, 0);
  ESP_LOGI(TAG, "Audio out at %d Hz", have.freq);
  return ESP_OK;
}

esp_err_t audio_write(const int16_t *pcm, size_t samples) {
  if (!device)
    return ESP_ERR_INVALID_STATE;
  if (!pcm && samples)
    return ESP_ERR_INVALID_ARG;

  if (SDL_QueueAudio(device, pcm, (Uint32)(samples * sizeof(int16_t))) != 0) {
    ESP_LOGW(TAG, "Queue failed: %s", SDL_GetError());
    return ESP_FAIL;
  }

  while (SDL_GetQueuedAudioSize(device) > queue_ahead_bytes())
    SDL_Delay(5);
  return ESP_OK;
}

void audio_stop(void) {
  if (device)
    SDL_ClearQueuedAudio(device);
}

void audio_deinit(void) {
  if (!device)
    return;
  SDL_ClearQueuedAudio(device);
  SDL_CloseAudioDevice(device);
  device = 0;
}
