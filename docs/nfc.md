# NFC Card Storage

> **Proof of concept. Do not put real seeds on these cards.**
>
> This is an experiment in a fork, written to find out whether the idea works
> at all — not a reviewed, audited or finished feature. It has been exercised
> on one board with one card type and has had no security review by anyone.
>
> Anything built for real use should start from the established practice for
> this problem rather than from this code: NFC tag security, key diversification
> instead of factory keys, replay and cloning resistance, and the physical
> threat model of a backup that answers any reader that comes near it. None of
> that is settled here.
>
> Kern itself already warns that it is a research project and unsuited to
> mainnet. This branch is further from ready than that. It is published so the
> approach can be looked at and argued with; the author takes no responsibility
> for what anyone else does with it.

Keeps KEF-encrypted seed backups and wallet output descriptors on NFC cards,
read and written through an external M5Stack RFID Unit 2 (WS1850S) on the
board's I2C bus. The card is a third destination alongside flash and SD: same
envelope, same password prompt, different medium.

**Off by default.** The driver is compiled in, but nothing happens until the
toggle under **Settings → NFC** is switched on.

---

## The air-gap question

`CONTRIBUTING.md` says never to introduce network or radio functionality, and
`main.c` holds the ESP32-C6 in reset for exactly that reason. NFC is a radio, so
this feature sits in tension with that rule and is deliberately narrow:

- The RF field is energized only inside the tap page, and dropped when it
  closes. `nfc_tap_page_destroy()` is also called from the session-lock path, so
  a device that locks while a card page is open does not keep an antenna live.
- The reader is attached to the I2C bus lazily, in that same page. With the
  toggle off, `nfc_init()` never runs.
- The seed never crosses the antenna in the clear. It is converted to compact
  SeedQR entropy, encrypted, and wiped before the reader is touched, and no
  path in Kern writes a mnemonic to a card unsealed.
- A descriptor may cross in the clear, at the user's explicit choice and behind
  a warning. That is a deliberate difference: a descriptor is public data by
  design — it is what you hand a coordinator — where a seed is not. The privacy
  cost is real and named below.
- The module is external. Unplugged, the feature reports "no reader" and stops.

What it is not: a network interface. There is no routing, no pairing and no
session — the far side is a memory tag a couple of centimetres away.

---

## Wiring

The reader hangs off whatever bus `bsp_i2c_get_handle()` returns, at address
`0x28`. That bus is shared with the touch controller and PMIC on most boards;
the RFID2 does not collide with either.

| Board | SDA | SCL | Notes |
|-------|-----|-----|-------|
| `wave_35` | GPIO7 | GPIO8 | Tested. Shares the bus with FT5x06 (0x38) and AXP2101 (0x34) |
| `wave_4b`, `wave_5`, `wave_43`, `crowpanel` | per BSP | per BSP | Untested, but no code change needed — see below |

Module: M5Stack RFID Unit 2 (WS1850S),
<https://shop.m5stack.com/products/rfid-unit-2-ws1850s>. Its Grove HY2.0-4P
lead carries SDA (yellow), SCL (white), VCC (red) and GND (black).

Supply is **3.3 V**, not the 5 V the Grove connector is nominally rated for.
The WS1850S is a 3.3 V part and the P4's logic is 3.3 V, so the whole link runs
at one level. Read range at the lower supply has not been measured — if a card
reads unreliably, suspect the supply first.

### Porting

There is nothing board-specific to port. `nfc_init()` takes an
`i2c_master_bus_handle_t` rather than opening a bus of its own, and every Kern
BSP already exposes `bsp_i2c_get_handle()`. Attaching the module to another
board's I2C pins is the whole job; the component compiles for all five boards
today.

---

## Supported tags

| Family | SAK | Usable bytes | Notes |
|--------|-----|--------------|-------|
| MIFARE Classic 1K | `0x08`, `0x88` | 752, capped at 720 | Ships with the RFID2 kit |
| MIFARE Classic 4K | `0x18` | as 1K | Upper sectors have a different layout and are not used |
| Ultralight / NTAG21x | `0x00` | 48–888, capped at 720 | NTAG213 is 144, NTAG215 is 504 |

Classic sectors are authenticated with the factory key A
(`FF FF FF FF FF FF`). The protection is the KEF password, not the sector key —
the card stays readable by any reader, and what a reader finds is ciphertext.

Any other SAK is treated as an empty field.
### An NDEF-formatted tag has to be wiped first

Sectors are authenticated with the factory key A (`FF FF FF FF FF FF`). A tag
that has been NDEF-formatted no longer uses it: the NFC Forum mapping puts
`A0 A1 A2 A3 A4 A5` on the MAD sector and `D3 F7 D3 F7 D3 F7` on the data
sectors, so authentication fails and the tag reads as if it held no backup.

This is not hypothetical — an off-the-shelf NFC ring arrived NDEF-formatted
with a Lightning address on it and behaved exactly that way. Erasing it with
any tag tool (NFC Tools' format/erase, for one) restores the factory keys and
it works immediately afterwards. Note that this destroys whatever NDEF content
the tag shipped with.

In the serial log the two cases are distinguishable: a key failure logs
`Header read failed: ESP_ERR_TIMEOUT`, while a readable tag with no Kern record
logs `Header rejected: not a Kern record`. The on-screen message is the same
for both, which is a rough edge worth knowing about.
 A KEF-wrapped 24-word seed is well
under 100 bytes, so every supported tag has room to spare.

---

## Card format

A 16-byte header at linear offset 0, payload immediately after:

```
0..3    magic "KRN1"
4       record type
5       reserved, must be zero
6..7    payload length, big endian
8..15   reserved, must be zero
```

### What a card can hold

| Type | Record | Written from | Read from |
|------|--------|--------------|-----------|
| 1 | KEF envelope (seed) | Back Up → Save to NFC | Load Mnemonic → From NFC Card |
| 2 | Wallet descriptor, sealed or plaintext | Session Descriptors → *pick one* → Save to NFC Card | Load Descriptor → From NFC Card |
| 3 | Datum | *reserved* | *reserved* |
| 4 | Extended public key | *reserved* | *reserved* |

The magic tags the format, not the device, so these numbers are shared with the
[Krux fork](https://github.com/sandman21vs/krux) that writes the same cards.
Kern writes 1 and 2. Types 3 and 4 are **reserved rather than unused**: Krux
writes them, so a card carrying one has a meaning Kern must not reassign later.
Kern refuses them on read — a datum card reads as no record — but the numbers
are pinned by a golden-vector test so a future feature cannot quietly take one.

### A type is a parser, not a permission

Each record carries its type, and a reader names the type it can parse. A
record of any other type reads exactly as a blank card does — so a descriptor
card offered to the mnemonic loader, or a seed card offered to the wallet, is
refused at the header, before anything is allocated and before any password is
asked for.

What a type buys is a parser, and nothing more. Whatever comes off a card still
faces the same validation the same bytes would face arriving by QR or SD: raw
BIP-39 entropy of 16 or 32 bytes for a mnemonic, the descriptor parser and its
cosigner decision for a descriptor. NFC adds a medium, never a shortcut past
one.

`nfc_has_record()` is the exception and asks about *any* known type, because it
feeds the overwrite warning: a seed about to be buried under a descriptor is
something to lose whether or not the page doing the burying could read it.

Offsets are linear. `picc.c` maps them onto MIFARE Classic blocks (skipping
block 0 and every sector trailer) or Ultralight pages (starting at page 4), so
neither the record layer nor the pages know which kind of tag is present.

The backup ID is not stored separately — it already lives inside the KEF
envelope's header, and `storage_get_kef_display_name()` reads it from there.

There is no checksum in the record format. For a sealed payload none is needed:
the KEF envelope is authenticated, so a half-written or decaying card fails to
decrypt. An unsealed descriptor has to bring its own — see below.

### A descriptor card, sealed or not

A type-2 record holds **either** a KEF envelope **or** a bare descriptor
string, exactly as a descriptor on an SD card may be a `.kef` or a `.txt`.
Which one it is needs no flag: `kef_is_envelope()` reads a version byte that is
always below 32, at an offset where a descriptor always has a character of 0x20
or above, so the two can never be confused. The fork is unambiguous by
construction, not by luck.

Choosing plaintext costs two things the envelope was doing for free.

**Privacy.** A descriptor holds every xpub in the wallet, which is its whole
history and all of its future addresses, and an unsealed card hands them to any
reader brought near it. The equivalent `.txt` on an SD card is at least sitting
in a drawer. Choose plaintext for a card the way you would choose it for a
sheet of paper — which is why that path prompts and the sealed one does not.

**Authentication.** Kern writes the plaintext form with its BIP-380 checksum
and **refuses a card whose checksum does not match**. The check is Kern's, not
libwally's: libwally parses a descriptor whether or not the checksum agrees,
and `descriptor_to_unambiguous()` strips it before the validator ever sees it,
so it is verified in `nfc_load_descriptor.c` or nowhere. That matters more than
it sounds — only the xpubs carry integrity of their own, being base58check. A
flipped bit in a fingerprint, a derivation path, a threshold or a script
wrapper is accepted in silence and points the wallet at different addresses. A
damaged card says `Card data is damaged`, which is a different message from
`No descriptor on this card` on purpose: one means rewrite it, the other means
it was never there.

**Size** is the remaining limit, and here Kern differs from the Krux fork.
`kef_encrypt_page.c` seals everything with `KEF_V20_GCM_E4`, which does not
compress, so the envelope costs `1 + id_len + 1 + 3 + 12 + 4` bytes on top of
the string — 29 with the default 8-character ID, which is the descriptor's own
checksum. Against a 704-byte payload ceiling:

| Form | On-card bytes | Largest descriptor body |
|------|---------------|-------------------------|
| Plaintext + `#cksum` | body + 9 | **695** |
| Sealed (KEF v20) | body + 38 | **666** |

So on Kern **sealing costs 29 bytes rather than saving them** — the reverse of
Krux, which deflates before it encrypts and fits a large descriptor sealed that
will not fit in the clear. A 2-of-3 `wsh(sortedmulti(...))` runs about 450
bytes and fits either way; a taproot miniscript may fit neither. Switching this
path to `KEF_V21_GCM_Z_E4` would compress and stays readable by both firmwares,
but that version is chosen in a page shared with the seed backup, so it is left
for a change that can consider both. A card that cannot hold the descriptor
says so before the antenna comes up.

### A phone will show the card as empty

This is expected and says nothing about whether the write worked. The layout
above is a raw block record, not an NDEF message, and phone NFC apps look for
NDEF — so they report an empty or unformatted tag. (iPhones cannot read MIFARE
Classic at all.) To check a card, read it back with Kern.

NDEF was not used on purpose: it would cost a MAD sector, TLV framing and a
format step, to make a ciphertext blob marginally more legible to software that
has no use for it.

---

## Treating the card as hostile

A card is input a stranger chose every byte of, and it only has to be held near
the device. Each layer refuses anything that is not exactly what Kern writes.

**Reader FIFO** (`pcd_ws1850s.c`) — the reply length is picked by the tag.
Reading `FIFOLevelReg` (up to 64) into a smaller buffer is the classic MFRC522
overflow, so `pcd_transceive()` takes its buffer size as an in/out parameter and
aborts with `ESP_ERR_INVALID_SIZE` rather than truncating; truncation would also
leave the protocol out of step. `RxLastBits` is masked to 0–7 before it becomes
shift arithmetic, `ErrorReg` is checked every frame, and every wait is bounded
by both the reader's 25 ms timer and a wall-clock deadline.

**Selection** (`picc.c`) — cascade is capped at two levels, so a tag cannot
claim cascade forever; ten-byte UIDs are refused rather than guessed at. BCC is
verified per level. SAK is an allowlist. On NTAG the capacity byte is
attacker-written, and `size × 8` is exactly the kind of number that overflows if
believed, so it is taken only when the NFC Forum magic byte is present and
clamped at both ends regardless. `picc_write()` refuses block 0 and sector
trailers — a corrupted trailer bricks its sector permanently.

**Record header** (`nfc_record.c`) — validated before anything is allocated, and
stops at the first divergence: magic, then type, then reserved bytes (which must
be zero, denying the field as a covert channel), then length against both the
compile-time ceiling and the tag's real capacity. The allocation uses the
validated value, never the raw field, so a hostile card cannot drive a large
allocation. The type check intersects the caller's mask with the types this
build knows, so a caller cannot widen the allowlist past the format — even by
asking for all of them.

**Descriptor payloads** (`nfc_load_descriptor.c`) — a payload is refused unless
every byte is printable ASCII. `descriptor_loader_process_string()` takes a
`const char *`, so an embedded NUL in a payload a stranger chose would truncate
the descriptor to a shorter one that still parses — a different wallet, loaded
without a word. An unsealed payload then has to prove its BIP-380 checksum
before the parser sees it.

**After decryption** (`nfc_load_mnemonic.c`) — decrypting does not make the
bytes ours. KEF versions with a 16-bit hidden auth let a wrong password through
roughly once in 65536 tries, and a planted card could have been encrypted with a
password its author chose. So the payload passes one narrow gate:
`mnemonic_qr_compact_to_mnemonic()`, which accepts 16 or 32 bytes and verifies
the BIP39 checksum. `mnemonic_qr_to_mnemonic()` is deliberately not used — it
would also accept plaintext words and numeric SeedQR, formats Kern never writes
to a card and therefore formats no genuine card can present.

### What this does not protect

A planted card the user accepts, with a password they get right, loads the
attacker's seed. That is the same exposure as a malicious QR code, and the same
defence applies: the fingerprint confirmation screen before the key is used.
The filtering above is about the path to that screen being free of memory
corruption, not about deciding whose seed it is.

The header parser is pure and has no I/O, so it runs on the host — including
the golden vectors that pin the record header byte for byte against Krux:

```bash
make -C components/nfc/test run
```

The descriptor checksum gate has its own vectors, computed from the BIP-380
spec rather than from the code under test:

```bash
make -C main/core/test run
```

---

## Layout

```
components/nfc/
  include/nfc.h          public API — all that main/ sees
  src/pcd_ws1850s.c      reader: I2C registers, ISO14443A framing
  src/picc.c             tags: selection, linear addressing
  src/nfc_record.c       header build/parse/validate (pure, host-testable)
  src/nfc.c              record I/O
  test/                  parser test suite

main/pages/nfc/
  nfc_tap_page.c         "hold a card" prompt, RF field lifecycle
  nfc_store_mnemonic.c   KEF encrypt, then write
  nfc_load_mnemonic.c    read, decrypt, confirm
  nfc_store_descriptor.c seal or checksum, then write
  nfc_load_descriptor.c  read, unseal or verify, validate

main/pages/login/nfc_settings.c   toggle and reader probe
```

Hooks into existing code are short blocks, all under `#if CONFIG_KERN_NFC` and
all hidden at runtime when the setting is off: the four menu entries (store and
load, for a seed and for a descriptor), the settings entry, the session-lock
teardown, and `nfc` in `main/CMakeLists.txt`. `main/core/storage.c` is
untouched — a card holds one record, so there is no file list to browse and no
third `storage_location_t`, and keeping that type out of the NFC page
signatures is what keeps it that way.

`descriptor_loader_show_source_menu()` gained an `nfc_cb` parameter rather than
a `#if`: a shared function whose *shape* depends on a config is the one thing
worth avoiding here, so the signature is the same in every build and a caller
that has nothing to offer passes NULL.

The simulator does not define `CONFIG_KERN_NFC` and lists its sources
explicitly, so it builds exactly as before.

### Where NFC is deliberately not offered

The "load a descriptor" offer on the PSBT review screen lists QR, Flash and SD
but not NFC. That path holds the PSBT under review in RAM for the whole detour,
and a card read plus a KEF decrypt is a 704-byte allocation followed by PBKDF2
with its own working buffers — the one place in the firmware where running
short loses a transaction rather than a menu. Energizing the antenna in the
middle of a signing review is also a posture change that wants its own
argument. Load a card-held descriptor from the Descriptor Manager first.

---

## Configuration

`CONFIG_KERN_NFC` (default y) compiles the feature in. `CONFIG_KERN_NFC_I2C_ADDR`
(default `0x28`) and `CONFIG_KERN_NFC_I2C_TIMEOUT_MS` (default 100) are under
**NFC Card Storage** in `menuconfig`.

To build without it:

```bash
idf.py -B build_wave_35 -D SDKCONFIG=build_wave_35/sdkconfig -D CONFIG_KERN_NFC=n build
```
