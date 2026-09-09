// Where the baked voice lives — see assets/speech_lexicon.h
//
// The blob is a megabyte of ADPCM and is linked in rather than compiled from
// source. The two platforms have different ways of doing that, and this is the
// only place that difference shows.

#include "assets/speech_lexicon.h"

#ifdef SIMULATOR

/* Emitted into the build directory by tools/bin2c.py; see
 * simulator/CMakeLists.txt. Never committed. */
extern const unsigned char speech_lexicon_bin[];

const uint8_t *speech_lexicon_blob(void) {
  return (const uint8_t *)speech_lexicon_bin;
}

#else

/* Placed by EMBED_FILES in main/CMakeLists.txt. */
extern const uint8_t lexicon_start[] asm("_binary_speech_lexicon_bin_start");

const uint8_t *speech_lexicon_blob(void) { return lexicon_start; }

#endif
