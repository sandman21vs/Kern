// IMA ADPCM decoding — the format the spoken lexicon is baked in

#ifndef ADPCM_H
#define ADPCM_H

#include <stddef.h>
#include <stdint.h>

/* Decoder state. A clip carries no state of its own: every clip in the bank
 * starts from a reset decoder, which is what tools/bake_speech.py encodes
 * against, so clips can be played in any order. */
typedef struct {
  int32_t predictor;
  int8_t index;
} adpcm_state_t;

void adpcm_reset(adpcm_state_t *state);

/**
 * Expand `count` 4-bit codes into signed 16-bit samples. Codes are packed two
 * to a byte, low nibble first.
 *
 * @param data  at least (count + 1) / 2 bytes
 * @param pcm   at least `count` samples
 */
void adpcm_decode(adpcm_state_t *state, const uint8_t *data, size_t count,
                  int16_t *pcm);

#endif /* ADPCM_H */
