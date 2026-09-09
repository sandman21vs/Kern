// Turning interface text into the clips that say it

#ifndef SPEECH_TEXT_H
#define SPEECH_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* Longest word the lookup will consider. Anything longer cannot be in the
 * bank, so it is spelled, which is the right answer for it anyway. */
#define SPEECH_MAX_WORD 32

/**
 * Index of a word in the baked lexicon, or -1.
 *
 * @param word  not required to be NUL-terminated or lowercase
 * @param len   bytes of `word` to consider
 */
int speech_word_index(const char *word, size_t len);

/**
 * Lay out the clips that speak `text`.
 *
 * Words in the bank become one clip. Words that are not - an acronym the bake
 * deliberately left out, a descriptor fragment, anything the interface gained
 * since the last bake - are spelled from the letter clips. Digits are read out
 * one at a time. Punctuation and icon glyphs are passed over silently.
 *
 * @param out  filled with lexicon indices
 * @param max  capacity of `out`; the utterance is truncated to fit
 * @return     number of clips written
 */
size_t speech_text_to_clips(const char *text, uint16_t *out, size_t max);

#endif /* SPEECH_TEXT_H */
