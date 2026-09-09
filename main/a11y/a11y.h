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

/** Stop talking now. The session lock and the screensaver use this. */
void a11y_silence(void);

#endif /* A11Y_H */
