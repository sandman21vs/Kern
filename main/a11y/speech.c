// The voice — see speech.h
//
// One task owns the speaker. Callers hand it an utterance and return; the task
// decodes clip by clip and pushes samples at components/audio, which blocks
// until the DMA has taken them.
//
// Interruption is the whole design. A one-item queue always holds the newest
// announcement; the task checks it every 16 ms and abandons stale speech.

#include "speech.h"

#include <string.h>

#include "adpcm.h"
#include "assets/speech_lexicon.h"
#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "speech_text.h"

#ifndef SIMULATOR
#include <bsp/esp-bsp.h>
#endif

static const char *TAG = "SPEECH";

/* Longest utterance laid out at once. A dialog body is the long case; past
 * this it is truncated, which is better than a reader that will not stop. */
#define MAX_CLIPS 96

/* Samples decoded per audio_write(). Even, so each chunk starts on a byte
 * boundary in the ADPCM stream. 16 ms of sound: short enough that an
 * interruption is inaudible, long enough not to thrash the I2S queue. */
#define CHUNK_SAMPLES 256

#define TASK_STACK 4096
#define TASK_PRIORITY 4
#define TASK_CORE 1

typedef struct {
  size_t count;
  uint16_t clips[MAX_CLIPS];
} utterance_t;

static QueueHandle_t commands;
static TaskHandle_t task;
static volatile bool running;
static volatile bool task_exited;
static bool ready;

/* ---------- Earcons ---------- */

/* Synthesised rather than baked: they are a few hundred samples of sine and
 * would otherwise be five more entries in a bank measured in megabytes. */
typedef struct {
  uint16_t hz;
  uint16_t ms;
} tone_t;

static const tone_t EARCONS[][2] = {
    [SPEECH_EARCON_FOCUS] = {{1200, 18}, {0, 0}},
    [SPEECH_EARCON_BOUNDARY] = {{440, 60}, {0, 0}},
    [SPEECH_EARCON_ACTIVATE] = {{880, 35}, {1320, 45}},
    [SPEECH_EARCON_ERROR] = {{320, 90}, {240, 110}},
    [SPEECH_EARCON_MASKED] = {{660, 40}, {660, 40}},
};
#define EARCON_COUNT (sizeof(EARCONS) / sizeof(EARCONS[0]))

/* A quarter-cycle of sine, enough to build a full one by symmetry. Integer
 * only: no libm, and no float on a task that runs constantly. */
static int16_t sine(uint32_t phase) {
  /* phase is 0..65535 over one cycle. Three-term parabola approximation,
   * within about 1% of sin() — inaudible on a beep. */
  int32_t x = (int32_t)(phase & 0xFFFF) - 32768; /* -32768..32767 */
  int32_t y = (x * (98304 - ((x < 0 ? -x : x) * 3))) >> 16;
  return (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
}

/* ---------- Playback ---------- */

static bool take_replacement(utterance_t *next) {
  if (xQueueReceive(commands, next, 0) != pdTRUE)
    return false;
  /* A chunk can land just after submit() clears the device. Clear it again
   * here so none of the superseded word leaks into the new announcement. */
  audio_stop();
  return true;
}

static bool play_clip(uint16_t index, utterance_t *next) {
  if (index >= SPEECH_CLIP_COUNT)
    return false;

  const speech_clip_t clip = speech_clips[index];
  const uint8_t *codes = speech_lexicon_blob() + clip.offset;
  adpcm_state_t state;
  adpcm_reset(&state);

  int16_t pcm[CHUNK_SAMPLES];
  size_t left = clip.samples;
  while (left > 0) {
    if (take_replacement(next))
      return true;
    const size_t take = left < CHUNK_SAMPLES ? left : CHUNK_SAMPLES;
    adpcm_decode(&state, codes, take, pcm);
    if (audio_write(pcm, take) != ESP_OK)
      return false;
    codes += (take + 1) / 2; /* take is even except on the last chunk */
    left -= take;
  }
  return false;
}

static bool play_tone(const tone_t *tone, utterance_t *next) {
  if (tone->hz == 0 || tone->ms == 0)
    return false;

  const uint32_t total = (uint32_t)AUDIO_SAMPLE_RATE * tone->ms / 1000;
  const uint32_t step = (uint32_t)tone->hz * 65536 / AUDIO_SAMPLE_RATE;
  const uint32_t fade = total / 8; /* keep the ends from clicking */
  uint32_t phase = 0;

  int16_t pcm[CHUNK_SAMPLES];
  for (uint32_t done = 0; done < total;) {
    if (take_replacement(next))
      return true;
    const uint32_t take =
        (total - done) < CHUNK_SAMPLES ? (total - done) : CHUNK_SAMPLES;
    for (uint32_t i = 0; i < take; i++) {
      const uint32_t at = done + i;
      int32_t sample = sine(phase) / 4; /* earcons sit under speech */
      if (at < fade)
        sample = sample * (int32_t)at / (int32_t)fade;
      else if (at > total - fade)
        sample = sample * (int32_t)(total - at) / (int32_t)fade;
      pcm[i] = (int16_t)sample;
      phase += step;
    }
    if (audio_write(pcm, take) != ESP_OK)
      return false;
    done += take;
  }
  return false;
}

/* Earcons are pushed as clip indices offset past the lexicon, so one queue
 * carries both and an earcon interrupts speech exactly like a word does. */
#define EARCON_BASE 0xF000

static void speech_task(void *arg) {
  (void)arg;
  utterance_t utterance;

  while (running) {
    if (xQueueReceive(commands, &utterance, portMAX_DELAY) != pdTRUE)
      continue;
    if (!running)
      break;

    bool replaced;
    do {
      replaced = false;
      for (size_t i = 0; i < utterance.count && !replaced; i++) {
        const uint16_t clip = utterance.clips[i];
        if (clip >= EARCON_BASE) {
          const size_t which = clip - EARCON_BASE;
          if (which >= EARCON_COUNT)
            continue;
          replaced = play_tone(&EARCONS[which][0], &utterance);
          if (!replaced)
            replaced = play_tone(&EARCONS[which][1], &utterance);
        } else {
          replaced = play_clip(clip, &utterance);
        }
      }
      /* take_replacement() has put the new command in `utterance`. */
    } while (running && replaced);

    if (!replaced)
      audio_stop();
  }

  task_exited = true;
  vTaskDelete(NULL);
}

/* ---------- Public API ---------- */

static void submit(const uint16_t *clips, size_t count) {
  if (!ready)
    return;

  utterance_t utterance = {.count = count};
  if (count)
    memcpy(utterance.clips, clips, count * sizeof(clips[0]));
  /* Clear first. If the task writes one last old chunk before seeing the new
   * command, replacement() clears that late chunk as well. */
  audio_stop();
  if (xQueueOverwrite(commands, &utterance) != pdPASS)
    ESP_LOGW(TAG, "Voice command dropped");
}

bool speech_init(void) {
  if (ready)
    return true;

#ifdef SIMULATOR
  const esp_err_t ret = audio_init(NULL);
#else
  const esp_err_t ret = audio_init(bsp_i2c_get_handle());
#endif
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "No speaker: %s", esp_err_to_name(ret));
    return false;
  }

  commands = xQueueCreate(1, sizeof(utterance_t));
  if (!commands) {
    speech_deinit();
    return false;
  }

  running = true;
  task_exited = false;
  if (xTaskCreatePinnedToCore(speech_task, "speech", TASK_STACK, NULL,
                              TASK_PRIORITY, &task, TASK_CORE) != pdPASS) {
    running = false;
    speech_deinit();
    return false;
  }

  ready = true;
  ESP_LOGI(TAG, "Voice ready, %d words", SPEECH_CLIP_COUNT);
  return true;
}

void speech_deinit(void) {
  ready = false;
  if (task) {
    const utterance_t stop = {0};
    running = false;
    audio_stop();
    xQueueOverwrite(commands, &stop);
    /* Do not free the queue underneath the task. Usually this is one tick;
     * unlike the old fixed delay, it also remains safe on a busy device. */
    while (!task_exited)
      vTaskDelay(1);
    task = NULL;
  }
  if (commands) {
    vQueueDelete(commands);
    commands = NULL;
  }
  audio_deinit();
}

bool speech_available(void) { return ready; }

void speech_say(const char *text) {
  if (!ready || !text)
    return;
  uint16_t clips[MAX_CLIPS];
  const size_t count = speech_text_to_clips(text, clips, MAX_CLIPS);
  if (count)
    submit(clips, count);
}

void speech_earcon(speech_earcon_t earcon) {
  if (!ready || (unsigned)earcon >= EARCON_COUNT)
    return;
  const uint16_t clip = (uint16_t)(EARCON_BASE + earcon);
  submit(&clip, 1);
}

void speech_silence(void) {
  if (!ready)
    return;
  submit(NULL, 0);
}
