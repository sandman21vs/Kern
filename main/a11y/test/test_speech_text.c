/*
 * Spoken-text layout test suite
 *
 * The interesting cases are the ones where the bank comes up short. A screen
 * reader that goes quiet on an unfamiliar word is worse than one that spells
 * it, and the bank is baked from the interface as it was on the day it was
 * baked - every string added after that lands here.
 *
 * The ADPCM checks are cross-implementation vectors: the codes were produced
 * by adpcm_encode() in tools/bake_speech.py, and the samples are what its
 * adpcm_decode() returns for them. If the C decoder and the Python encoder
 * ever disagree, the whole lexicon decodes to noise and nothing else in the
 * tree would notice.
 *
 * Compile with: make
 * Run: make run
 */

#include "adpcm.h"
#include "speech_lexicon.h"
#include "speech_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("Testing: %s... ", name)
#define PASS()                                                                 \
  do {                                                                         \
    printf("PASS\n");                                                          \
    tests_passed++;                                                            \
  } while (0)
#define FAIL(msg)                                                              \
  do {                                                                         \
    printf("FAIL: %s\n", msg);                                                 \
    tests_failed++;                                                            \
  } while (0)
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      PASS();                                                                  \
    } else {                                                                   \
      FAIL(msg);                                                               \
    }                                                                          \
  } while (0)

#define MAX_CLIPS 64

static size_t say(const char *text, uint16_t *out) {
  return speech_text_to_clips(text, out, MAX_CLIPS);
}

/* Does the clip sequence spell exactly this string? */
static int spells(const uint16_t *clips, size_t n, const char *letters) {
  if (n != strlen(letters))
    return 0;
  for (size_t i = 0; i < n; i++) {
    const char one[2] = {letters[i], '\0'};
    if (clips[i] != (uint16_t)speech_word_index(one, 1))
      return 0;
  }
  return 1;
}

static void test_lookup(void) {
  TEST("the bank is sorted, so lookup can bisect");
  int sorted = 1;
  for (int i = 1; i < SPEECH_CLIP_COUNT; i++)
    if (strcmp(speech_clips[i - 1].word, speech_clips[i].word) >= 0)
      sorted = 0;
  CHECK(sorted, "speech clips are not strictly ascending");

  TEST("every word in the bank finds itself");
  int found = 1;
  for (int i = 0; i < SPEECH_CLIP_COUNT; i++)
    if (speech_word_index(speech_clips[i].word, strlen(speech_clips[i].word)) !=
        i)
      found = 0;
  CHECK(found, "a baked word did not look up to its own index");

  TEST("lookup is case-insensitive");
  CHECK(speech_word_index("SAVE", 4) == speech_word_index("save", 4),
        "uppercase did not match");

  TEST("a word that is not there reports so");
  CHECK(speech_word_index("zzzznotaword", 12) == -1, "expected -1");

  TEST("an over-long word cannot be in the bank");
  char long_word[SPEECH_MAX_WORD + 8];
  memset(long_word, 'a', sizeof(long_word));
  CHECK(speech_word_index(long_word, sizeof(long_word)) == -1,
        "expected -1 rather than a buffer overrun");

  TEST("every letter has a clip, so spelling always has something to say");
  int letters = 1;
  for (char c = 'a'; c <= 'z'; c++)
    if (speech_word_index(&c, 1) < 0)
      letters = 0;
  CHECK(letters, "a letter is missing from the bank");

  TEST("every digit name has a clip");
  const char *names[] = {"zero", "one", "two",   "three", "four",
                         "five", "six", "seven", "eight", "nine"};
  int digits = 1;
  for (int i = 0; i < 10; i++)
    if (speech_word_index(names[i], strlen(names[i])) < 0)
      digits = 0;
  CHECK(digits, "a digit name is missing from the bank");
}

static void test_layout(void) {
  uint16_t clips[MAX_CLIPS];

  TEST("a known phrase is one clip per word");
  CHECK(say("Save to SD", clips) ==
            1 + 1 + 2 /* save, to, then s and d spelled */,
        "unexpected clip count");

  TEST("an interface word maps to its own clip");
  CHECK(say("save", clips) == 1 &&
            clips[0] == (uint16_t)speech_word_index("save", 4),
        "wrong clip for a baked word");

  TEST("an acronym left out of the bank is spelled");
  CHECK(say("psbt", clips) == 4 && spells(clips, 4, "psbt"),
        "psbt should come out as four letters");

  TEST("a word the bank has never seen is spelled, not dropped");
  const size_t n = say("zzq", clips);
  CHECK(n == 3 && spells(clips, 3, "zzq"), "an unknown word went silent");

  TEST("digits are read out one at a time");
  CHECK(say("12", clips) == 2 &&
            clips[0] == (uint16_t)speech_word_index("one", 3) &&
            clips[1] == (uint16_t)speech_word_index("two", 3),
        "digits did not become number words");

  TEST("punctuation says nothing");
  CHECK(say("save!!! ... to", clips) == 2, "punctuation reached the speaker");

  TEST("an icon glyph in a label is passed over");
  /* A label often carries a Font Awesome codepoint next to its text. */
  CHECK(say("\xef\x80\x82 save", clips) == 1, "the glyph bytes were spoken");

  TEST("empty and null text say nothing");
  CHECK(say("", clips) == 0 &&
            speech_text_to_clips(NULL, clips, MAX_CLIPS) == 0,
        "expected silence");

  TEST("a long utterance is truncated rather than overrunning");
  uint16_t small[3];
  CHECK(speech_text_to_clips("save save save save save", small, 3) == 3,
        "the cap was not respected");

  TEST("zero capacity is handled");
  CHECK(speech_text_to_clips("save", clips, 0) == 0, "expected 0");
}

static void test_adpcm(void) {
  adpcm_state_t state;
  int16_t pcm[128];

  /* Every vector below was produced by adpcm_decode() in
   * tools/bake_speech.py, the mirror of the encoder that baked the bank. */

  TEST("the largest positive code climbs exactly as the encoder expects");
  const uint8_t up[] = {0x77, 0x77, 0x77, 0x77};
  const int16_t up_expected[] = {11, 41, 104, 240, 533, 1164, 2521, 5431};
  adpcm_reset(&state);
  adpcm_decode(&state, up, 8, pcm);
  CHECK(memcmp(pcm, up_expected, sizeof(up_expected)) == 0,
        "the step table, the delta shifts or the packing disagree");

  TEST("the sign bit mirrors it downward");
  const uint8_t down[] = {0xFF, 0xFF};
  const int16_t down_expected[] = {-11, -41, -104, -240};
  adpcm_reset(&state);
  adpcm_decode(&state, down, 4, pcm);
  CHECK(memcmp(pcm, down_expected, sizeof(down_expected)) == 0,
        "code 0xF should be 0x7 negated");

  TEST("the low nibble is the earlier sample");
  /* 0x70 is a zero-magnitude code followed by the largest one. Read in the
   * wrong order it would step first and sit still second. */
  const uint8_t packed[] = {0x70};
  adpcm_reset(&state);
  adpcm_decode(&state, packed, 2, pcm);
  CHECK(pcm[0] == 0 && pcm[1] == 11, "nibbles came out swapped");

  TEST("the predictor saturates instead of wrapping");
  uint8_t loud[64];
  memset(loud, 0x77, sizeof(loud));
  adpcm_reset(&state);
  adpcm_decode(&state, loud, 128, pcm);
  CHECK(pcm[10] == 32767 && pcm[127] == 32767,
        "expected a clamp at full scale");

  TEST("decoding zero codes touches nothing");
  adpcm_reset(&state);
  adpcm_decode(&state, up, 0, pcm);
  CHECK(state.predictor == 0 && state.index == 0, "state moved");

  TEST("state carries across calls, so a clip can be decoded in pieces");
  adpcm_reset(&state);
  adpcm_decode(&state, up, 4, pcm);
  adpcm_decode(&state, up + 2, 4, pcm + 4);
  CHECK(memcmp(pcm, up_expected, sizeof(up_expected)) == 0,
        "splitting a clip changed what it decodes to");
}

static void test_bank(void) {
  TEST("clip offsets stay inside the blob");
  int inside = 1;
  for (int i = 0; i < SPEECH_CLIP_COUNT; i++) {
    const uint32_t bytes = (speech_clips[i].samples + 1) / 2;
    if (speech_clips[i].offset + bytes > SPEECH_CLIP_BYTES)
      inside = 0;
  }
  CHECK(inside, "a clip runs off the end of the blob");

  TEST("clips are laid out end to end with no gaps");
  int packed = speech_clips[0].offset == 0;
  for (int i = 1; i < SPEECH_CLIP_COUNT; i++) {
    const uint32_t prev =
        speech_clips[i - 1].offset + (speech_clips[i - 1].samples + 1) / 2;
    if (speech_clips[i].offset != prev)
      packed = 0;
  }
  CHECK(packed, "the blob has holes in it");

  TEST("no clip is empty");
  int nonempty = 1;
  for (int i = 0; i < SPEECH_CLIP_COUNT; i++)
    if (speech_clips[i].samples == 0)
      nonempty = 0;
  CHECK(nonempty, "a word rendered to nothing");
}

/* The blob is not linked into the host suite - it is a binary asset the device
 * embeds - so this reads it off disk. The point is to prove that an offset in
 * the index, the ADPCM decoder and the bytes on disk agree: a clip that
 * decodes to silence or to noise would sound like a broken feature and look
 * like a working build. */
static void test_real_clip(void) {
  TEST("a word from the bank decodes to the samples the baker produced");
  FILE *f = fopen("../assets/speech_lexicon.bin", "rb");
  if (!f) {
    FAIL("cannot open ../assets/speech_lexicon.bin");
    return;
  }

  const int index = speech_word_index("save", 4);
  const speech_clip_t clip = speech_clips[index];
  const size_t bytes = (clip.samples + 1) / 2;
  uint8_t *codes = malloc(bytes);
  int16_t *pcm = malloc(clip.samples * sizeof(int16_t));
  if (!codes || !pcm) {
    FAIL("out of memory");
    free(codes);
    free(pcm);
    fclose(f);
    return;
  }

  int ok = fseek(f, clip.offset, SEEK_SET) == 0 &&
           fread(codes, 1, bytes, f) == bytes;
  fclose(f);
  if (ok) {
    adpcm_state_t state;
    adpcm_reset(&state);
    adpcm_decode(&state, codes, clip.samples, pcm);

    /* Values from tools/bake_speech.py decoding the same clip. */
    const int16_t head[] = {0, -7, 9, 11, -16, -6, 25, 29};
    int peak = 0;
    for (uint16_t i = 0; i < clip.samples; i++) {
      const int magnitude = pcm[i] < 0 ? -pcm[i] : pcm[i];
      if (magnitude > peak)
        peak = magnitude;
    }
    ok = clip.samples == 4731 && memcmp(pcm, head, sizeof(head)) == 0 &&
         pcm[clip.samples / 2] == 14022 && peak == 27420;
  }
  free(codes);
  free(pcm);
  CHECK(ok, "the clip on disk did not decode to what the bake produced");
}

int main(void) {
  printf("=== Spoken text layout tests ===\n\n");
  test_lookup();
  test_layout();
  test_adpcm();
  test_bank();
  test_real_clip();
  printf("\nPassed: %d, Failed: %d\n", tests_passed, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
