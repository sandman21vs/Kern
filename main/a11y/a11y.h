// The screen reader — reading the interface with a finger

#ifndef A11Y_H
#define A11Y_H

#include <stdbool.h>

/**
 * Wrap the touch input device and start the voice. Idempotent, and cheap when
 * the reader is off: the wrapper is installed either way and passes touches
 * straight through until enabled.
 *
 * Call after both the display and theme_init(): the slop and double-tap
 * distances come from theme_min_touch_size(), and before the theme is resolved
 * that is zero, which makes every touch a drag and no two taps ever near
 * enough to count as a double.
 *
 * @return false if there is no speaker, in which case the reader stays off.
 */
bool a11y_init(void);

/** Turn reading on or off. Off is the default and passes every touch through
 *  untouched, so a sighted user never notices the wrapper is there. */
void a11y_set_enabled(bool enabled);

bool a11y_enabled(void);

/**
 * Say something the interface wants said - a page that just opened, a dialog,
 * a menu entry that was picked. Silent when the reader is off.
 */
void a11y_announce(const char *text);

/**
 * Allow masked content - seed words, PIN digits, a passphrase - to be read
 * aloud. Off by default and worth leaving off: the board has a speaker and no
 * headphone jack, so everything it says can be overheard or recorded.
 *
 * It exists because the alternative is worse. With it off, someone who cannot
 * see the screen cannot enter a PIN or check a seed backup at all; the reader
 * plays a tone where the digits are and nothing else. Turning it on is a
 * decision about the room they are in, and only they can make it.
 */
void a11y_set_speak_secrets(bool allowed);

bool a11y_speak_secrets(void);

/** Stop talking now. The session lock and the screensaver use this. */
void a11y_silence(void);

#endif /* A11Y_H */
