<p align="center">
  <img src="branding/kern_logo_with_text_dark_bg.png" alt="Kern" width="400">
</p>

<p align="center">
  <a href="https://sandman21vs.github.io/Kern/"><b>Fork website</b></a> ·
  <a href="https://sandman21vs.github.io/Kern/flash/"><b>Fork Web Flasher</b></a> ·
  <a href="docs/nfc.md"><b>NFC notes</b></a> ·
  <a href="https://github.com/odudex/Kern"><b>Official Kern</b></a>
</p>

> [!CAUTION]
> # ⚠ Experimental fork, not official Kern
>
> **This is [sandman21vs/Kern](https://github.com/sandman21vs/Kern/tree/nfc-card-storage), an unofficial fork of [odudex/Kern](https://github.com/odudex/Kern) used as a proof-of-concept test of NFC card storage:** KEF-encrypted seed backups and wallet descriptors written to NFC cards through an external M5Stack RFID Unit 2. See [docs/nfc.md](docs/nfc.md).
>
> - **Do not use it with real seeds or real funds.** Testnet and throwaway keys only.
> - **Not reviewed or endorsed by the Kern maintainers**, never audited, and tested on one board with one card type.
> - **NFC is a radio.** This fork deliberately weakens Kern's air-gap to find out whether the idea works at all.
> - **Builds from this fork, including the [fork web flasher](https://sandman21vs.github.io/Kern/flash/), are unvetted snapshots of an experimental branch.** They may misbehave, change without notice, or lose what is stored on the device.
> - **No warranty, no support, no responsibility** for what anyone does with it. Report problems with this fork here, not upstream.
>
> For the real project, use the [official website](https://odudex.github.io/Kern/), the [official web flasher](https://odudex.github.io/Kern/flash/) and the [Telegram group](https://t.me/kern_custody).

---

Kern is a research and development project exploring what new hardware can do for Bitcoin self-custody. It takes the form of an air-gapped signing device on the ESP32-P4: the chip has no radio, so keys are generated and used on hardware that physically cannot reach a network. Transactions cross the air gap as QR codes or over an SD card.

The goal is to explore ideas: new silicon, new interfaces, new backup and signing workflows. They get implemented, tested and documented in the open. Kern is a research platform and intends to stay one; it is not on a path to becoming a product.

It signs PSBTs for single-sig, multisig and miniscript policies on both native segwit and taproot, built on [libwally](https://github.com/ElementsProject/libwally-core/), the same core library used by Blockstream Jade.

> **Warning:** Kern is a research and development project, built for experimentation and testnet use. It has not been audited and secure boot is not enabled by default; builds are unvetted development snapshots. Any mainnet use is entirely at your own risk. The project makes no security guarantees.

## Hardware

Kern supports five Waveshare ESP32-P4 boards and one Elecrow CrowPanel board:

| Board | Display | Touch | Camera |
|-------|---------|-------|--------|
| [ESP32-P4-WiFi6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm) (`wave_4b`) | 720x720 MIPI DSI | GT911 | sold separately, OV5647 with autofocus recommended |
| [ESP32-P4-WiFi6-Touch-LCD-3.5](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-3.5.htm) (`wave_35`) | 320x480 SPI | FT5x06 | OV5647, included |
| [ESP32-P4-WiFi6-Touch-LCD-5](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-5.htm) (`wave_5`) | 720x1280 MIPI DSI | GT911 | OV5647, included |
| [ESP32-P4-WiFi6-Touch-LCD-4.3](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4.3.htm) (`wave_43`) | 480x800 MIPI DSI | GT911 | OV5647, included |
| [ESP32-P4-WiFi6-Touch-LCD-7B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-7b.htm) (`wave_7b`) | 1024x600 MIPI DSI | GT911 | OV5647, sold separately |
| [CrowPanel Advanced 10.1" ESP32-P4](https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen) and 7" siblings (`crowpanel`) | 1024x600 MIPI DSI | GT911 | SC2336, included |

ESP32-P4 does not contain radio (WiFi, BLE), but these boards have a radio in a secondary chip (ESP32-C6 mini). Exploring radio-less, simpler and cheaper ESP32-P4-only boards is part of the project's hardware research.

A MIPI CSI camera module is required for all boards. Kern ships drivers for the
OV5647 and SC2336 sensors and probes for whichever one is attached at boot, so
either sensor works on any board. The 4B is the one board sold without a camera:
pair it with an OV5647 module carrying a DW9714 voice coil motor, which is the
only combination that gets autofocus.

## Prerequisites

Kern targets [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/get-started/index.html). ESP-IDF needs a few system packages first (git, Python 3, CMake, Ninja). Install them with:

- **macOS:** `brew install cmake ninja dfu-util python3`
- **Debian/Ubuntu:** `sudo apt install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0`
- **Windows:** use the [ESP-IDF Windows installer](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/get-started/windows-setup.html), or build inside WSL2 with the Debian/Ubuntu steps.

Then install ESP-IDF itself for the `esp32p4` target, at the path the `just` recipes expect (`~/esp/esp-idf`):

```bash
git clone --depth 1 --recurse-submodules --shallow-submodules -b v6.1 https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32p4
. ~/esp/esp-idf/export.sh
```

## Build

### Quick start (this fork)

The NFC experiment lives only on the **`nfc-card-storage`** branch. The fork's `master` is the default branch and is kept identical to upstream Kern, so a plain clone (without `-b nfc-card-storage`) builds firmware **without** NFC. You need ESP-IDF (see [Prerequisites](#prerequisites)) and [just](#installing-just). Then plug in the board over USB and run:

```bash
git clone --recursive -b nfc-card-storage https://github.com/sandman21vs/Kern.git
cd Kern
. ~/esp/esp-idf/export.sh
just build wave_35
just flash wave_35
```

Replace `wave_35` with your board's target from the [Hardware](#hardware) table. `just` loads ESP-IDF from `~/esp/esp-idf` by itself. If you call `idf.py` directly, run `. ~/esp/esp-idf/export.sh` first in every new terminal.

After flashing, NFC is still **off**. Wire the reader as described in [docs/nfc.md](docs/nfc.md#wiring), then switch it on under **Settings → NFC**.

To pull later changes to the experiment:

```bash
git pull
git submodule update --init --recursive
```

### Cloning the Repository

This project uses git submodules, and the NFC code is on the `nfc-card-storage` branch. You have two options:

#### Option 1: Clone the NFC branch with submodules (Recommended)

```bash
git clone --recursive -b nfc-card-storage https://github.com/sandman21vs/Kern.git
```

#### Option 2: Fix up an existing clone

If you already cloned without `--recursive` or without `-b nfc-card-storage`, switch to the branch and fetch the submodules:

```bash
git checkout nfc-card-storage
git submodule update --init --recursive
```

Run `git branch --show-current` to check which branch you are on. It should print `nfc-card-storage`.

> **Note:** The submodules (`libwally-core`, `cUR`, `k_quirc`) come from the upstream author's repositories and are the same as in official Kern. Only this repository is forked.

### Installing just

The `just` commands below need [just](https://github.com/casey/just). Install it once:

```bash
brew install just
```

On Debian/Ubuntu 24.04+ run `sudo apt install just`. On other systems, see [just's install guide](https://github.com/casey/just#installation). If you'd rather not install it, the `idf.py` commands below do the same thing.

### Building the Project

Build with [just](https://github.com/casey/just) (recommended) or `idf.py` directly. All `just` commands accept a board parameter, one of `wave_4b` (default), `wave_35`, `wave_5`, `wave_43`, `crowpanel`, or `wave_7b`:

```bash
just build              # Build for wave_4b (default)
just build wave_35      # Build for wave_35
just build wave_5       # Build for wave_5
just build wave_43      # Build for wave_43
just build crowpanel    # Build for CrowPanel 7" / 10.1"
just build wave_7b      # Build for wave_7b
just flash wave_5       # Flash for wave_5
just monitor            # Serial monitor
just clean              # Wipe all build_<board> dirs + sdkconfig
```

Or using `idf.py` directly:

```bash
# wave_4b
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.wave_4b' build

# wave_35
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.wave_35' build

# wave_5
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.wave_5' build

# wave_43
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.wave_43' build

# crowpanel (7" / 10.1")
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.crowpanel' build

# wave_7b
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.wave_7b' build
```

> **Note:** `just` builds each board into its own `build_<board>/` directory, so switching boards needs no clean. The raw `idf.py` commands above share the default `build/` directory and `sdkconfig`: run `idf.py fullclean && rm sdkconfig` when switching boards that way.

> **Note:** The first build auto-generates `dev_signing_key.pem` (gitignored) and every build is signed with it. This is required to boot: the firmware refuses to run unsigned images. This key is a per-clone development key that carries no trust and never leaves your machine. Official releases are signed offline with the project's release key instead, so a self-built device only accepts SD-card updates built from the same clone; flash releases over USB.

### Desktop Simulator

The simulator renders the full LVGL UI in an SDL2 window, matching each board's resolution:

```bash
just sim                # Run simulator as wave_4b (720x720) with webcam (V4L2)
just sim wave_35        # Run simulator as wave_35 (320x480)
just sim wave_5         # Run simulator as wave_5 (720x1280)
just sim wave_43        # Run simulator as wave_43 (480x800)
just sim crowpanel      # Run simulator as crowpanel (1024x600)
just sim wave_7b        # Run simulator as wave_7b (1024x600)
just sim-build wave_35  # Build only
just sim-clean          # Remove simulator build artifacts
just sim-reset          # Wipe simulator data (factory reset)
just sim-qr IMG         # Run with a QR image
just sim-no-cam         # Run without camera
```

Switching simulator boards also requires `just sim-clean` first. See [simulator/README.md](simulator/README.md) for details.

### Full Clean

After updating ESP-IDF or switching branches with significant build changes, do a full clean to avoid stale artifacts:

```bash
idf.py fullclean
rm sdkconfig
idf.py set-target esp32p4
idf.py build
```

### Build Options

#### Enable/disable Auto-focus

To enable camera auto-focus, enable camera focus motor on menuconfig:

```
CONFIG_CAM_MOTOR_DW9714=y
CONFIG_CAMERA_OV5647_ENABLE_MOTOR_BY_GPIO0=y
```

## Web Flasher

The easiest way to flash Kern is the browser-based flasher, which requires no local toolchain. It works in **Google Chrome** or **Microsoft Edge** (version 89+) via the Web Serial API.

**This fork's flasher (NFC experiment):** https://sandman21vs.github.io/Kern/flash/

**Official Kern flasher (no NFC):** https://odudex.github.io/Kern/flash/

The flasher offers two modes:
- **Latest CI Build**: fetches firmware built by the most recent push to `nfc-card-storage` directly from the site and flashes it to the selected board.
- **Custom ZIP Bundle**: accepts a `firmware-<board>.zip` artifact downloaded from the [Actions tab](../../actions) to flash any PR or older build.

> **Warning:** CI builds are unvetted development snapshots from a research project. Secure boot is not enabled. Flash them for experimentation and testnet use; any mainnet use is entirely at your own risk.

> **Note:** In this fork, the site and flasher are deployed automatically on every successful push to `nfc-card-storage`, from `site/` in this repository. Because `master` is the default branch and mirrors upstream, the Actions tab has no *Run workflow* button for this; redeploy by hand with `gh workflow run test-all-builds.yml --ref nfc-card-storage`. The `github-pages` environment only accepts deploys from `nfc-card-storage`, so syncing `master` with upstream never replaces the fork's flasher. To enable it for your fork, go to **Settings → Pages** and set the source to **GitHub Actions**. To preview the site locally, run `just site` and open http://localhost:8000. Web Serial works on localhost, so you can flash a real board from the local copy.

## Flashing CI Build Artifacts

Every pull request and push to `nfc-card-storage` or `master` produces a firmware artifact for each supported board via the **Test All Builds** workflow. These builds are useful for testing unreleased changes without setting up a local toolchain.

### Requirements

- Python 3
- USB cable connected to the board

### Steps

1. Open the **Actions** tab of the repository on GitHub and select the workflow run you want.

2. Scroll to the **Artifacts** section at the bottom of the run summary and download the zip for your board (e.g. `firmware-wave_4b`).

3. Unzip the package:

   ```bash
   unzip firmware-wave_4b.zip -d firmware-wave_4b
   cd firmware-wave_4b
   ```

   The zip contains, among other files:
   - `bootloader.bin`: bootloader
   - `partition-table.bin`: partition table
   - `ota_data_initial.bin`: OTA data partition
   - `kern.bin`: application firmware
   - `flasher_args.json` / `flash_args`: pre-computed flash offsets

4. Create a Python virtual environment and install esptool:

   ```bash
   python3 -m venv venv
   source venv/bin/activate
   pip install esptool
   ```

5. Flash using the pre-computed offsets from `flash_args`:

   ```bash
   esptool --chip esp32p4 --baud 460800 write-flash $(cat flash_args)
   ```

   > **Note:** Artifacts built before September 2026 ship a `flash_args` that still points at the `bootloader/` and `partition_table/` build subdirectories, which the flat zip does not contain. For those, pass the offsets explicitly:
   >
   > ```bash
   > esptool --chip esp32p4 --baud 460800 write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x2000 bootloader.bin 0x10000 partition-table.bin 0x1e000 ota_data_initial.bin 0x20000 kern.bin
   > ```

## Flashing Pre-releases

Pre-release firmware is provided **for research and testing purposes only**. Any mainnet use is entirely at your own risk.

> **Note:** This fork does not publish releases. The steps below flash **official Kern** releases, which do not include the NFC experiment. To run the NFC build, use the [fork web flasher](https://sandman21vs.github.io/Kern/flash/) or [build it yourself](#quick-start-this-fork).

### Supported Devices

| Device | Board | Display |
|--------|-------|---------|
| `wave_4b` | Waveshare ESP32-P4-WiFi6-Touch-LCD-4B | 720x720 MIPI DSI |
| `wave_35` | Waveshare ESP32-P4-WiFi6-Touch-LCD-3.5 | 320x480 SPI |
| `wave_5` | Waveshare ESP32-P4-WiFi6-Touch-LCD-5 | 720x1280 MIPI DSI |
| `wave_43` | Waveshare ESP32-P4-WiFi6-Touch-LCD-4.3 | 480x800 MIPI DSI |
| `crowpanel` | CrowPanel Advanced 7" / 10.1" ESP32-P4 | 1024x600 MIPI DSI |
| `wave_7b` | Waveshare ESP32-P4-WiFi6-Touch-LCD-7B | 1024x600 MIPI DSI |

### Requirements

- Python 3
- USB cable connected to the board

### Steps

1. Download the zip for your device from the [Releases](https://github.com/odudex/Kern/releases) page (e.g. `kern-wave_4b-v0.0.3.zip`).

2. Unzip the package:

```bash
unzip kern-wave_4b-v0.0.3.zip
```

The zip contains:
- `bootloader.bin`: bootloader
- `partition-table.bin`: partition table
- `firmware-signed.bin`: application firmware (signed; also the file for SD-card updates)
- `kern-v0.0.3.hex`: single-file image (all of the above, Intel HEX)

3. Create a Python virtual environment and install esptool:

```bash
python3 -m venv venv
source venv/bin/activate
pip install esptool
```

4. Flash the single-file image:

```bash
esptool --chip esp32p4 --baud 460800 write-flash 0x0 kern-v0.0.3.hex
```

> **Note:** The `.hex` image is sparse: it writes only the regions it contains, so the NVS partition (PIN, settings) and stored data survive reflashing. For a factory-clean install, erase everything first: `esptool --chip esp32p4 erase-flash`, then flash the image.

### Updating via SD card

A device already running signed firmware updates without any computer: copy `firmware-signed.bin` from the zip onto a SD card, insert it, and go to **Settings → Firmware Update**. The device verifies the signature before installing and keeps the previous firmware as an automatic fallback.

## Community

Report problems with the NFC experiment, or anything else specific to this fork, on [this fork's issues](https://github.com/sandman21vs/Kern/issues). Please do not file them upstream.

Kern itself is discussed in the official Telegram group, [t.me/kern_custody](https://t.me/kern_custody), and on [odudex/Kern](https://github.com/odudex/Kern/issues).

## References

Kern is strongly inspired by [Krux](https://github.com/selfcustody/krux), sharing similar but simplified UI elements and flow.

[Blockstream Jade](https://github.com/Blockstream/Jade) was a strong inspiration for the decision to use C language for efficient use of the hardware. Additionally, Kern uses the same core library Jade does, [libwally](https://github.com/ElementsProject/libwally-core/), is shared with Jade.

The simplicity and UI polish of [SeedSigner](https://github.com/SeedSigner/seedsigner) and the security focus of the pioneering [Specter-DIY](https://github.com/cryptoadvance/specter-diy) were also strong inspirations.

## [Roadmap](ROADMAP.md)

## [Contributing](CONTRIBUTING.md)

## License

[MIT](LICENSE)
