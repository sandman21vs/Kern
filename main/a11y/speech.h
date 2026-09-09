// The voice — a queue in front of the speaker

#ifndef SPEECH_H
#define SPEECH_H

#include <stdbool.h>

/* Short non-speech sounds. Cheaper than a word and, for the things a reader
 * says constantly, clearer: a listener learns a tick faster than they can sit
 * through "selected" a hundred times. */
typedef enum {
  SPEECH_EARCON_FOCUS,    /* the finger moved onto something */
  SPEECH_EARCON_BOUNDARY, /* there is nothing further this way */
  SPEECH_EARCON_ACTIVATE, /* the double tap landed */
  SPEECH_EARCON_ERROR,    /* something refused */
  SPEECH_EARCON_MASKED,   /* there is text here that will not be spoken */
} speech_earcon_t;

/**
 * Bring up the codec and start the speech task. Idempotent.
 *
 * @return false when there is no speaker circuit to talk to, in which case
 *         every call below is a no-op and the reader should stay off.
 */
bool speech_init(void);

/** Stop the task and put the codec away. Safe without a preceding init. */
void speech_deinit(void);

/** True once speech_init() has succeeded. */
bool speech_available(void);

/**
 * Say `text`, interrupting whatever is being said.
 *
 * Interrupting is the point: a finger sliding across the screen produces a new
 * announcement every few hundred milliseconds, and a reader that queued them
 * would fall further behind the finger with every one.
 *
 * Returns immediately; the words are laid out here and played on the speech
 * task. Never call from that task.
 */
void speech_say(const char *text);

/** Play a short sound, interrupting speech the same way. */
void speech_earcon(speech_earcon_t earcon);

/** Drop what is playing and what is pending. */
void speech_silence(void);

#endif /* SPEECH_H */
