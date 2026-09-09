#!/usr/bin/env python3
"""Bake the spoken lexicon Kern's screen reader plays.

The interface is the vocabulary. This scans main/ for the strings that reach
the user through the menu, dialog and page-title helpers, reduces them to the
set of distinct words, renders each one with a host text-to-speech engine, and
emits main/a11y/assets/speech_lexicon.{c,h}.

Words rather than phrases, because a signer has to say things no phrase bank
can hold: an amount, an index, a word being typed. Anything not in the bank is
spelled from the same letter clips.

The generated files are committed, the way tools/bake_icons.py's output is, so
no build ever needs a speech engine. Re-run this only when the interface gains
words:

    ./scripts/bake_speech.sh
"""

import argparse
import array
import collections
import math
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import wave

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "main" / "a11y" / "assets"

# The helpers every user-visible string passes through. Keep in step with
# main/ui/menu.h, main/ui/dialog.h and main/ui/theme_widgets.h.
CALLS = [
    "ui_menu_add_entry", "ui_menu_add_entry_with_icon",
    "ui_menu_add_entry_with_action", "ui_menu_add_entry_with_icon_and_action",
    "ui_menu_set_entry_label", "theme_create_page_title", "theme_create_button",
    "theme_create_label", "dialog_show_info", "dialog_show_acknowledge",
    "dialog_show_error_timeout", "dialog_show_confirm",
    "dialog_show_danger_confirm", "dialog_show_message", "dialog_show_progress",
]

# Said by the reader itself, so they are never found by scanning the interface.
GLUE = [
    "on", "off", "selected", "disabled", "dimmed", "button", "menu", "dialog",
    "checked", "unchecked", "heading", "of", "item", "empty", "screen",
    "top", "bottom", "hidden", "reader", "speaking", "spelling",
]

DIGITS = ["zero", "one", "two", "three", "four", "five", "six", "seven",
          "eight", "nine"]

LETTERS = [chr(c) for c in range(ord("a"), ord("z") + 1)]

# Read out letter by letter. A speech engine makes an unpronounceable mess of
# these, and a listener needs the letters anyway. Initialisms that happen to be
# ordinary words - "pin", "ok" - are deliberately absent: they are said, not
# spelled, and "pin" alone reaches the user on every boot.
ACRONYMS = {
    "qr", "psbt", "kef", "sd", "nfc", "xpub", "bip", "id",
    "usb", "url", "utxo", "sha", "aes", "hmac", "csv", "json", "rgb",
}

LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')

# ---------- IMA ADPCM ----------
# The reference tables. main/a11y/adpcm.c decodes with the same two.

STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(samples):
    """Encode signed 16-bit mono to 4-bit IMA ADPCM, two codes per byte."""
    pred, index, out, pending = 0, 0, bytearray(), None
    for sample in samples:
        step = STEP_TABLE[index]
        diff = sample - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            code |= 4
            diff -= step
            delta += step
        step >>= 1
        if diff >= step:
            code |= 2
            diff -= step
            delta += step
        step >>= 1
        if diff >= step:
            code |= 1
            delta += step
        pred = max(-32768, min(32767, pred - delta if code & 8 else pred + delta))
        index = max(0, min(88, index + INDEX_TABLE[code]))
        if pending is None:
            pending = code
        else:
            out.append(pending | (code << 4))
            pending = None
    if pending is not None:
        out.append(pending)
    return bytes(out)


def adpcm_decode(data, count):
    """The encoder's mirror, used here only to measure what was lost."""
    pred, index, out = 0, 0, []
    for i in range(count):
        code = data[i >> 1] & 0x0F if i % 2 == 0 else data[i >> 1] >> 4
        step = STEP_TABLE[index]
        delta = step >> 3
        if code & 4:
            delta += step
        if code & 2:
            delta += step >> 1
        if code & 1:
            delta += step >> 2
        pred = max(-32768, min(32767, pred - delta if code & 8 else pred + delta))
        index = max(0, min(88, index + INDEX_TABLE[code]))
        out.append(pred)
    return out


# ---------- Vocabulary ----------

def call_arguments(text, name):
    """Argument text of each call to `name`, matching parentheses properly so a
    call broken over several lines is not missed."""
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        i, depth = match.end(), 1
        while i < len(text) and depth:
            char = text[i]
            if char == '"':
                i += 1
                while i < len(text) and text[i] != '"':
                    i += 2 if text[i] == "\\" else 1
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            i += 1
        yield text[match.end():i - 1]


def interface_words():
    words = collections.Counter()
    literals = 0
    for path in sorted((ROOT / "main").rglob("*.c")):
        text = path.read_text(errors="replace")
        for name in CALLS:
            for args in call_arguments(text, name):
                for literal in LITERAL.findall(args):
                    if not literal.strip():
                        continue
                    literals += 1
                    for word in re.findall(r"[A-Za-z]+", literal.replace("\\n", " ")):
                        words[word.lower()] += 1
    return words, literals


# ---------- Rendering ----------

def render(word, voice, rate, tmpdir):
    """Speak one word to trimmed 16 kHz mono samples."""
    path = tmpdir / "clip.wav"
    subprocess.run(
        ["say", "-v", voice, "-r", str(rate),
         "--data-format=LEI16@16000", "-o", str(path), word],
        check=True, capture_output=True)
    with wave.open(str(path)) as handle:
        assert handle.getnchannels() == 1 and handle.getframerate() == 16000
        samples = array.array("h")
        samples.frombytes(handle.readframes(handle.getnframes()))
    path.unlink()

    if not samples:
        return samples
    peak = max(abs(min(samples)), abs(max(samples)))
    if peak == 0:
        return array.array("h")
    # Drop the engine's leading and trailing silence: at a word a time it is
    # most of the file, and it is the difference between speech that runs and
    # speech that plods.
    floor = max(200, peak // 50)
    first = next((i for i, v in enumerate(samples) if abs(v) > floor), 0)
    last = next((i for i in range(len(samples) - 1, -1, -1)
                 if abs(samples[i]) > floor), len(samples) - 1)
    pad = 16000 // 200  # 5 ms, so a plosive is not clipped off
    samples = samples[max(0, first - pad):min(len(samples), last + pad + 1)]

    # Normalise to a common level, short of full scale so concatenation cannot
    # clip, then fade the ends so joins do not tick.
    gain = (32767 * 0.85) / peak
    fade = 16000 // 500  # 2 ms
    out = array.array("h", [0]) * len(samples)
    for i, value in enumerate(samples):
        scaled = int(value * gain)
        if i < fade:
            scaled = scaled * i // fade
        elif i >= len(samples) - fade:
            scaled = scaled * (len(samples) - 1 - i) // fade
        out[i] = max(-32768, min(32767, scaled))
    return out


# ---------- Emission ----------

HEADER = """/*******************************************************************************
 * Spoken lexicon for the screen reader
 * Voice: {voice} at {rate} wpm, 16 kHz mono, 4-bit IMA ADPCM
 * Generated locally, do not hand-edit:
 *   scripts/bake_speech.sh
 ******************************************************************************/

"""


def emit(entries, voice, rate):
    """Write the index as C and the audio as a binary blob.

    The audio does not become a C array. At 16 kHz it is over a megabyte, and
    spelled out as hex that is a seven-megabyte source file - eighty times the
    largest file in the tree - for a compiler to parse on every clean build.
    The blob is committed as-is and linked in: EMBED_FILES on the device,
    tools/bin2c.py at build time for the simulator.
    """
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    blob, clips = bytearray(), []
    for word, samples in entries:
        clips.append((word, len(blob), len(samples)))
        blob.extend(adpcm_encode(samples))

    (OUT_DIR / "speech_lexicon.bin").write_bytes(bytes(blob))

    (OUT_DIR / "speech_lexicon.h").write_text(
        HEADER.format(voice=voice, rate=rate) + f"""#ifndef SPEECH_LEXICON_H
#define SPEECH_LEXICON_H

#include <stdint.h>

/* One spoken word. `offset` indexes the clip blob in bytes, `samples` is the
 * decoded length; two ADPCM codes live in each byte. */
typedef struct {{
  const char *word;
  uint32_t offset;
  uint16_t samples;
}} speech_clip_t;

#define SPEECH_CLIP_COUNT {len(clips)}
#define SPEECH_SAMPLE_RATE 16000
#define SPEECH_CLIP_BYTES {len(blob)}

/* Sorted by word, so a lookup can bisect. */
extern const speech_clip_t speech_clips[SPEECH_CLIP_COUNT];

/* The ADPCM blob, linked in per platform. See main/a11y/speech_blob.c. */
const uint8_t *speech_lexicon_blob(void);

#endif /* SPEECH_LEXICON_H */
""")

    lines = [HEADER.format(voice=voice, rate=rate),
             '#include "speech_lexicon.h"\n\n',
             "const speech_clip_t speech_clips[SPEECH_CLIP_COUNT] = {\n"]
    lines += [f'    {{"{word}", {offset}, {samples}}},\n'
              for word, offset, samples in clips]
    lines.append("};\n")
    (OUT_DIR / "speech_lexicon.c").write_text("".join(lines))
    return blob, clips


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--voice", default="Samantha")
    parser.add_argument("--rate", type=int, default=240,
                        help="words per minute; faster is smaller, and screen "
                             "reader users tend to prefer it")
    args = parser.parse_args()

    if not shutil.which("say"):
        sys.exit("error: no 'say' on PATH. This bake needs macOS; the output "
                 "it produces is committed so no other build does.")

    words, literals = interface_words()
    # Acronyms are left out of the bank on purpose: a speech engine makes a
    # mess of "psbt", and a missing word is spelled from the letter clips,
    # which is what a listener wants for those anyway.
    vocabulary = sorted((set(words) - ACRONYMS) | set(GLUE) | set(DIGITS)
                        | set(LETTERS))
    spelled = sorted(set(words) & ACRONYMS)
    print(f"{literals} interface strings -> {len(words)} words, "
          f"{len(vocabulary)} clips with letters, digits and glue")
    print(f"spelled rather than spoken: {' '.join(spelled)}")

    entries, worst_snr = [], 99.0
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = pathlib.Path(tmp)
        for n, word in enumerate(vocabulary, 1):
            samples = render(word, args.voice, args.rate, tmpdir)
            if not samples:
                print(f"  warning: {word!r} rendered silent, skipped")
                continue
            entries.append((word, samples))
            decoded = adpcm_decode(adpcm_encode(samples), len(samples))
            signal = math.sqrt(sum(float(v) * v for v in samples) / len(samples))
            noise = math.sqrt(sum(float(a - b) ** 2
                                  for a, b in zip(samples, decoded)) / len(samples))
            if noise > 0:
                worst_snr = min(worst_snr, 20 * math.log10(signal / noise))
            if n % 50 == 0:
                print(f"  {n}/{len(vocabulary)}")

    blob, clips = emit(entries, args.voice, args.rate)
    seconds = sum(c[2] for c in clips) / 16000
    print(f"\n{len(clips)} clips, {seconds:.1f}s of speech, "
          f"{len(blob) / 1024:.0f} KiB of ADPCM "
          f"({seconds / len(clips) * 1000:.0f} ms average)")
    print(f"worst clip signal-to-noise: {worst_snr:.1f} dB "
          f"(4-bit IMA ADPCM lands around 20-30 dB on speech)")


if __name__ == "__main__":
    main()
