// IMA ADPCM decoder — see adpcm.h
//
// The mirror of adpcm_encode() in tools/bake_speech.py. The two tables below
// are the ones that script encodes with; they are the format, not a tuning
// choice, and changing either here without changing it there silently turns
// the whole lexicon into noise.

#include "adpcm.h"

static const int16_t STEP_TABLE[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int8_t INDEX_TABLE[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                       -1, -1, -1, -1, 2, 4, 6, 8};

void adpcm_reset(adpcm_state_t *state) {
  state->predictor = 0;
  state->index = 0;
}

void adpcm_decode(adpcm_state_t *state, const uint8_t *data, size_t count,
                  int16_t *pcm) {
  int32_t predictor = state->predictor;
  int32_t index = state->index;

  for (size_t i = 0; i < count; i++) {
    const uint8_t byte = data[i >> 1];
    const uint8_t code =
        (i & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
    const int32_t step = STEP_TABLE[index];

    /* delta = step * (code_bits + 0.5) / 4, built by shifts so the decoder
     * matches the encoder exactly rather than approximately. */
    int32_t delta = step >> 3;
    if (code & 4)
      delta += step;
    if (code & 2)
      delta += step >> 1;
    if (code & 1)
      delta += step >> 2;

    predictor = (code & 8) ? predictor - delta : predictor + delta;
    if (predictor > 32767)
      predictor = 32767;
    else if (predictor < -32768)
      predictor = -32768;

    index += INDEX_TABLE[code];
    if (index < 0)
      index = 0;
    else if (index > 88)
      index = 88;

    pcm[i] = (int16_t)predictor;
  }

  state->predictor = predictor;
  state->index = (int8_t)index;
}
