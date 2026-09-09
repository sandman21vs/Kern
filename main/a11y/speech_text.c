// Interface text -> lexicon clips (see speech_text.h)
//
// Pure: no LVGL, no ESP-IDF, no audio. That is what lets main/a11y/test cover
// the part most likely to be wrong - what happens to a word the bank does not
// hold - without a board.

#include "speech_text.h"

#include <stdbool.h>
#include <string.h>

#include "assets/speech_lexicon.h"

/* Digits are spoken, not spelled, so they need the number words rather than
 * the character. The bake puts all ten in the bank. */
static const char *const DIGIT_WORDS[10] = {"zero",  "one",  "two", "three",
                                            "four",  "five", "six", "seven",
                                            "eight", "nine"};

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

int speech_word_index(const char *word, size_t len) {
  if (!word || len == 0 || len >= SPEECH_MAX_WORD)
    return -1;

  char key[SPEECH_MAX_WORD];
  for (size_t i = 0; i < len; i++)
    key[i] = lower(word[i]);
  key[len] = '\0';

  /* speech_words is sorted by the bake, so this bisects. */
  size_t low = 0, high = SPEECH_CLIP_COUNT;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    const int cmp = strcmp(key, speech_words[mid]);
    if (cmp == 0)
      return (int)mid;
    if (cmp < 0)
      high = mid;
    else
      low = mid + 1;
  }
  return -1;
}

/* Append one clip if there is room. Returns false once the utterance is full,
 * which stops the caller rather than letting it spin. */
static bool push(uint16_t *out, size_t max, size_t *n, int index) {
  if (index < 0)
    return true; /* nothing to say for this piece; not a failure */
  if (*n >= max)
    return false;
  out[(*n)++] = (uint16_t)index;
  return true;
}

static bool spell(const char *word, size_t len, uint16_t *out, size_t max,
                  size_t *n) {
  for (size_t i = 0; i < len; i++) {
    const char c = lower(word[i]);
    if (!push(out, max, n, speech_word_index(&c, 1)))
      return false;
  }
  return true;
}

size_t speech_text_to_clips(const char *text, uint16_t *out, size_t max) {
  size_t n = 0;
  if (!text || !out || max == 0)
    return 0;

  for (size_t i = 0; text[i];) {
    if (is_alpha(text[i])) {
      size_t len = 0;
      while (is_alpha(text[i + len]))
        len++;
      const int index = speech_word_index(text + i, len);
      const bool ok = (index >= 0) ? push(out, max, &n, index)
                                   : spell(text + i, len, out, max, &n);
      if (!ok)
        break;
      i += len;
    } else if (is_digit(text[i])) {
      const char *name = DIGIT_WORDS[text[i] - '0'];
      if (!push(out, max, &n, speech_word_index(name, strlen(name))))
        break;
      i++;
    } else {
      /* Punctuation, spaces, and the multi-byte icon glyphs that share a label
       * with its text. None of them are said. */
      i++;
    }
  }
  return n;
}
