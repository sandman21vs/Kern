/*******************************************************************************
 * Spoken lexicon for the screen reader
 * Voice: Samantha at 240 wpm, 16 kHz mono, 4-bit IMA ADPCM
 * Generated locally, do not hand-edit:
 *   scripts/bake_speech.sh
 ******************************************************************************/

#ifndef SPEECH_LEXICON_H
#define SPEECH_LEXICON_H

#include <stdint.h>

/* One spoken word. `offset` indexes the clip blob in bytes, `samples` is the
 * decoded length; two ADPCM codes live in each byte. */
typedef struct {
  const char *word;
  uint32_t offset;
  uint16_t samples;
} speech_clip_t;

#define SPEECH_CLIP_COUNT 339
#define SPEECH_SAMPLE_RATE 16000
#define SPEECH_CLIP_BYTES 1016294

/* Sorted by word, so a lookup can bisect. */
extern const speech_clip_t speech_clips[SPEECH_CLIP_COUNT];

/* The ADPCM blob, linked in per platform. See main/a11y/speech_blob.c. */
const uint8_t *speech_lexicon_blob(void);

#endif /* SPEECH_LEXICON_H */
