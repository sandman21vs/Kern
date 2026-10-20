#include "../utils/attributes.h"
#pragma once

#include <stdbool.h>

struct wally_descriptor;

KERN_WARN_UNUSED_RESULT bool
descriptor_string_from_descriptor(const struct wally_descriptor *desc,
                                  char **output);
KERN_WARN_UNUSED_RESULT bool
descriptor_checksum_from_descriptor(const struct wally_descriptor *desc,
                                    char out[9]);

/* True when `text` ends in "#xxxxxxxx" and those eight characters are the
 * BIP-380 checksum of everything before the '#'.
 *
 * Computed over the literal body, not over a canonical form: the checksum is
 * defined on the string as written, and anything Kern or Krux writes already
 * carries a checksum that agrees with its own body. Normalizing first would
 * reject a valid descriptor spelled with "'" instead of "h".
 *
 * This exists because libwally parses a descriptor whether or not its checksum
 * agrees, so a caller holding bytes from a medium with no integrity of its own
 * — an NFC card — has to check it here or never. Only the xpubs inside a
 * descriptor defend themselves, being base58check; a flipped bit in a
 * fingerprint, a derivation index, a threshold or a script wrapper is accepted
 * in silence and points the wallet at different addresses. */
KERN_WARN_UNUSED_RESULT bool descriptor_checksum_verify_text(const char *text);

/* True if `s` contains an uppercase 'H' as a hardened-derivation marker
 * (i.e. one or more digits at a path-component boundary — after '/', '<', or
 * ';' — followed by 'H'). libwally accepts 'H', 'h', and '\'' interchangeably,
 * but the canonical form used for dedup normalizes only 'h' and '\'', so
 * descriptors using 'H' must be rejected at the input boundary. */
KERN_WARN_UNUSED_RESULT bool
descriptor_text_has_uppercase_hardened(const char *s);
