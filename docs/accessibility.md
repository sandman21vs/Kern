# Screen Reader

> **Proof of concept. Nothing here has been tried by a blind user.**
>
> This is an experiment in a fork, written to find out whether an air-gapped
> signer can be operated without sight at all — not a reviewed, audited or
> finished feature. It has had no accessibility review and no usability
> testing, and the one test that would mean anything has not happened yet.
>
> Anything built for real use should start from the established practice for
> this problem rather than from this code: the WCAG and platform accessibility
> guidelines, how TalkBack and VoiceOver actually behave in the hands of people
> who depend on them, and the question of what a device with a speaker and no
> headphone jack should be willing to say out loud. None of that is settled
> here.

Kern is entirely visual: one LVGL screen, navigation by touch, no audio and no
focus traversal. This reads the interface aloud and lets it be navigated by
finger, on the model blind users already know from Android's TalkBack — drag to
hear what is under the finger, swipe to step through the screen in order, tap
twice to activate.

It is off by default and compiles out entirely.

## The overhearing question

A signer that talks is a signer that can be listened to. There is no headphone
jack on any supported board: the only output is a speaker header, so everything
the device says is said to the room.

That makes the interesting decision not "how do we read the screen" but "what
do we refuse to read". By default the reader will not say seed words, PIN
digits, or a passphrase. Where those are on screen it stops, so their presence
is known, and plays a short tone instead of their contents.

Which leaves a real problem rather than a solved one. With that refusal in
place, someone who cannot see the screen **cannot enter a PIN or check a seed
backup at all**. So there is a second setting, off by default and behind the
same red danger dialog the destructive flows use, that lifts it. Whether the
room is safe is a judgement only the person in it can make. The device's job is
to make sure it is a decision and not a default.

The keyboard is masked along with the field it feeds. Announcing each key would
put a PIN on the speaker one digit at a time; announcing keys with distinct
tones would put it there as a tune. Every key sounds identical.

## Wiring

The Waveshare boards carry an ES8311 codec and an NS4150B amplifier but ship
the speaker header empty. A speaker has to be plugged in before any of this can
be heard.

| Board | Codec | Speaker header |
|-------|-------|----------------|
| wave_35 | ES8311 @ 0x18 | MX1.25 2P, 8 Ω 2 W |
| wave_43 | ES8311 @ 0x18 | GH1.25 2P, 8 Ω 2 W |
| wave_4b | ES8311 @ 0x18 | MX1.25 2P, 8 Ω 2 W |
| wave_5 | ES8311 @ 0x18 | MX1.25 2P, 8 Ω 2 W |
| wave_7b | ES8311 @ 0x18 | PH2.0 2P, 8 Ω 2 W |
| crowpanel | not verified | — |

The pins are the same across the Waveshare family, which is why there is no
per-board table in the code: MCLK 13, BCLK 12, LRCK 10, data out 9, amplifier
enable 53, codec on the I2C bus Kern already opens for the display and camera.
They come from Kconfig, so a board that wires them differently is a config
change rather than a code change.

crowpanel is left out. Its codec has not been confirmed, so
`sdkconfig.defaults.crowpanel` sets `CONFIG_KERN_AUDIO=n` and neither the codec
driver nor the reader is compiled for it.

### Porting

`components/audio` takes an I2C bus handle rather than opening one, so it
carries no board dependency. A new board needs the five pin values in Kconfig
and nothing else, provided its codec is an ES8311 clocked from an MCLK pin.

## The voice

There is no text-to-speech engine on the device and no way to fetch one. The
voice is a bank of recorded words, baked on a development machine and committed:

- `tools/bake_speech.py` scans `main/` for the strings that reach the user
  through the menu, dialog, button, label and page-title helpers. 306 strings
  reduce to 299 distinct words.
- With the twenty-six letters, the ten digit names and the words the reader
  says itself, that is **339 clips, 127 seconds, 992 KiB** at 16 kHz in 4-bit
  IMA ADPCM. The 240 wpm voice starts and finishes words sooner while keeping
  every current interface word in the bank.
- The blob is linked in, not compiled: `EMBED_FILES` on the device,
  `tools/bin2c.py` at build time for the simulator. As a C array it would be a
  7.2 MB source file, eighty times the largest file in the tree.

The bank holds **words, not phrases**, because a signer has to say things no
phrase bank can hold — an amount, an index, a word being typed. A sentence is a
sequence of clips; anything the bank does not hold is spelled from the same
letter clips; digits are read out. Seven initialisms are deliberately left out
of the bank so they are spelled: `bip`, `id`, `kef`, `psbt`, `qr`, `sd`, `xpub`.
`pin` and `ok` are deliberately *not* on that list — they are ordinary words,
and "P. I. N. settings" on every boot would be a small daily insult.

Re-baking needs macOS, because it uses `say`. Nothing else does: the output is
committed, so no build and no CI ever needs a speech engine.

```
./scripts/bake_speech.sh          # then ./scripts/format.sh
```

## Navigating

| Gesture | What happens |
|---------|--------------|
| Drag a finger | Says what is under it, as it changes |
| Swipe right / left | Steps to the next / previous element |
| Tap twice | Activates what was last announced |
| A new page or dialog | Announces itself |

With the reader on, a touch explores rather than presses: the touch device's
read callback is wrapped and reports every touch to LVGL as released. A double
tap re-points the input at the announced widget's centre and lets a real press
through, so the real widget reacts — a synthesised click would not open a
dropdown or press a keyboard key, and the keyboard is the one screen this has
to get right.

With the reader off the wrapper is still installed and changes nothing.

## What is never spoken

Unless "Speak sensitive data" is explicitly turned on:

- **Seed words** on the word-list backup page
- **PIN digits**, and every key of the pad that enters them
- **Passphrases**, and the KEF encryption key
- anything else inside a field the codebase marks `password_mode`

Each of those produces a tone in place of its contents.

### What this does not cover

- `main/pages/shared/mnemonic_editor.c` and
  `main/pages/load_mnemonic/manual_input.c` show words while they are being
  chosen and are **not** masked yet. They need a mark on their word grids.
- The QR backup page renders the seed as an image. It is silent because there
  is no text to read, which is right by accident rather than by decision.
- Addresses, amounts and public keys are read aloud. That is deliberate —
  checking them by ear is most of the point of a talking signer — but it is
  still material an eavesdropper learns.
- Nothing here defends against a recording device in the room. It cannot.

## Layout

```
components/audio/                 ES8311 + I2S, playback only
  include/audio.h                 four calls: init, write, stop, deinit
  src/audio.c                     one clock configuration, no codec abstraction
  Kconfig                         pins, address, volume

main/a11y/
  a11y.{c,h}                      the touch intercept: explore, scan, activate
  describe.{c,h}                  widget -> words; names and masks
  speech.{c,h}                    the queue and the task that owns the speaker
  speech_text.{c,h}               text -> clips; spelling fallback   (pure)
  adpcm.{c,h}                     IMA ADPCM decoder                  (pure)
  speech_blob.c                   where the baked voice is linked from
  assets/speech_lexicon.{bin,c,h} generated; do not hand-edit
  test/                           27 host checks over the two pure layers

main/pages/login/a11y_settings.c  the two switches and "Test speaker"
simulator/platform/audio_sim/     the same audio.h, backed by SDL
tools/bake_speech.py              the bake
tools/bin2c.py                    the blob, for the simulator
```

Hook points in existing code, all guarded by `#if CONFIG_KERN_A11Y`:

| File | What it does |
|------|--------------|
| `main/ui/theme_widgets.c` | a page title announces the page |
| `main/ui/menu.c` | a menu announces its title — covers all 22 |
| `main/ui/dialog.c` | a dialog announces title and body as one utterance |
| `main/ui/input_helpers.c` | names the four corner buttons; masks password fields |
| `main/pages/home/backup/mnemonic_words.c` | masks the seed grid |
| `main/pages/session_lock.c` | silences the queue when the session expires |
| `main/main.c` | brings the reader up before the PIN gate |

No page file is touched to make the reader work. Descriptions are read out of
the widget tree rather than annotated onto it, so they cannot go stale.

## Configuration

`CONFIG_KERN_A11Y` (default y, depends on `CONFIG_KERN_AUDIO`) compiles the
reader and the 992 KiB voice. It costs 0x1e1000 → 0x2e1000 of the app partition,
leaving 0x31f000 (3.1 MiB) free.

Building it in does not turn it on. Two runtime settings, both off by default,
under **Settings → Accessibility** — which sits before the PIN gate, because
someone who cannot see the screen has to be able to turn the reader on before
being asked for a PIN:

- **Screen reader** — read the interface aloud
- **Speak sensitive data** — lift the mask, behind the danger dialog

To build without any of it:

```bash
idf.py -B build_wave_43 -D SDKCONFIG=build_wave_43/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.wave_43' \
  -D CONFIG_KERN_AUDIO=n build
```

## What has been tested

On the host, `main/a11y/test` covers the two pure layers with 27 checks: the
lexicon lookup and its spelling fallback, the ADPCM decoder against
cross-implementation vectors from the Python encoder that baked the bank, and a
real clip read out of the committed blob and decoded to the samples the baker
produced.

`simulator/tests/a11y_touch_smoke.c` covers the touch layer itself, built as
`kern_sim_a11y_smoke` by the simulator build: 33 checks against a real LVGL
tree with speech stubbed to a recorder. It lives there rather than in
`main/a11y/test` because it needs LVGL and the theme, which that suite
deliberately does without. Both input modes are exercised — the device polls
its input device, the simulator's SDL mouse is event-driven, and two of the
bugs it now guards against existed only in the second.

In the simulator, the speech chain has been exercised end to end against SDL —
339 words load, an utterance takes the wall-clock time its samples say it
should, and interrupting a five-word sentence cuts it in 205 ms — and the touch
state machine against a real LVGL tree with speech stubbed to a recorder, 22
checks covering exploration, the double tap, the swipe, the corner-button names
and the masking gate in both positions.

**Not tested: any of it, on hardware, out loud.** The speaker header ships
empty and nothing has been heard yet. The other five boards are built but not
run. crowpanel is not built with it at all.

And the test that matters has not happened: nobody who needs a screen reader
has tried this.
