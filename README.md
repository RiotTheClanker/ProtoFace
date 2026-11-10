# 🐾 ProtoFace — Protogen Visor Firmware

Firmware for a Protogen suit visor running on the **Raspberry Pi Pico 2 W**.  
Drives the LED panels described by a `display.json`, reads animations from an SD card,
reacts to a microphone, and is controlled wirelessly via **Bluetooth LE (NUS/UART profile)**.

[![Firmware build](https://github.com/RiotTheClanker/ProtoFace/actions/workflows/build.yml/badge.svg)](https://github.com/RiotTheClanker/ProtoFace/actions/workflows/build.yml)

Animations are made with the companion tool,
[Protogen AnimFile Maker](https://github.com/RiotTheClanker/Protogen_AnimFile_Maker).

---

## ✨ Features

| Feature | Details |
|---|---|
| 🧩 **Any panel layout** | One firmware for every helmet: `display.json` on the SD card describes the panels and wiring |
| 🌈 **NeoPixel outputs** | Up to 4 WS2812 chains on any free pins (HUB75 and MAX7219 planned) |
| 💾 **SD card animations** | Plays `.anim` files from SD — `start.anim` auto-loads at boot |
| 🎤 **Sound reactivity** | Three modes per LED: Static, Snap (threshold), Linear (volume-scaled), plus sound-triggered frames |
| 📡 **Bluetooth LE control** | NUS UART profile — connect with any BLE serial app |
| ⚡ **USB-C power budgeting** | HUSB238 PD negotiation + per-frame dimming to stay within the supply |
| 🔁 **Fallback face** | "NO SD" and warning triangles, drawn for whatever display is set up |
| ⌨️ **CLI over BLE** | `list`, `load`, `fallback`, `play`, `pause`, `next`, `prev`, `bright`, `status`, `power`, `reload`, `ident`, `display` |

---

## 🗂️ Repository Structure

```
ProtoFace/
├── src/
│   ├── main.cpp            # Main firmware — setup, loop, BLE, SD, power, animation engine
│   └── pf_config.{h,cpp}   # display.json parser, LED mapping, built-in NO SD face
├── displays/               # display.json files: mk2.json, mk3.json, examples/, schema
├── docs/display-config.md  # The display.json format
├── tests/host/             # Unit tests for pf_config, run on a PC / in CI
├── .github/workflows/
│   ├── build.yml           # CI — firmware build + display config tests on every PR / push
│   └── release.yml         # On a V* tag — builds and publishes the release
├── docs/releases/          # Release notes, one file per version tag
├── .gitignore              # Excludes .pio/ build cache and binaries
├── platformio.ini          # PlatformIO project config
└── README.md
```

---

## 🔧 Hardware Pinout

| Signal | GPIO | Notes |
|---|---|---|
| LED outputs | set in `display.json` | Mk2/Mk3 configs use GP2 (left) and GP3 (right) |
| SD card CS | GP17 | SPI chip-select |
| Microphone (analog) | GP26 | ADC0 — SPW2430 DC output |
| USB-C PD (HUSB238) SDA | GP4 | I2C0 — optional; without it a 0.5 A USB budget is assumed |
| USB-C PD (HUSB238) SCL | GP5 | I2C0 |

> SPI (SD card) uses SPI0: SCK=GP18, MOSI=GP19, MISO=GP16 (set explicitly in `setup()`).

---

## 🧩 Display Config (`display.json`)

The firmware has no built-in panel layout. The panels, their pins and their wiring are
described by a `display.json` (format: [`docs/display-config.md`](docs/display-config.md)).

1. Copy a config to the root of the SD card as `display.json`:
   [`displays/mk2.json`](displays/mk2.json) (11 panels, GP2 + GP3),
   [`displays/mk3.json`](displays/mk3.json) (14 panels), or one from
   [`displays/examples/`](displays/examples/) or your own.
2. Boot the helmet. A new or changed `display.json` is checked and copied into the Pico's
   flash, and the flash copy is used from then on, so the face keeps working without the
   card. A bad file is not used: `status` says why, and the old config stays.
3. To change the display, replace the file on the card and reboot (or send `reload`).

Without a config in flash nothing lights up. The SD card, microphone, USB-PD and wireless
pins (GP4–5, GP16–19, GP26, GP23–25, GP29) can't be used by a config.

`.anim` files must match the config: version 2 files carry a layout ID, version 1 files
(Mk2/Mk3) play on any config made of 11 or 14 8×8 panels.

### Checking the wiring

Send `ident`: every panel shows a digit, its position on its output's chain (0 = first
panel after the Pico), one colour per output, with a red dot on the panel's first LED.
If panels are in the wrong place or mounted turned, fix the `chain` order or the panel's
`flip`/`rotate` in `display.json`. Send `ident` again to go back to the face.

---

## 🔁 Fallback Face

The fallback face is shown at boot when there is no SD card or no `start.anim`, and
whenever a file fails to load. It is drawn from the display config: "NO SD" on the mouth
panels and warning triangles on the eyes and nose (a big panel without regions gets a triangle above "NO SD").

> **Both sides need artwork.** Each side of the face has its own panels in the file.
> In the tool, keep **Mirror to other side** ticked (or use **Copy side 1 → side 2**) so the
> right-hand chain isn't left black.

---

## 🚀 Getting Started

### Prerequisites

- [VSCode](https://code.visualstudio.com/) + [PlatformIO IDE](https://platformio.org/install/ide?install=vscode)
- **or** PlatformIO CLI: `pip install platformio`

### Build & Flash

```bash
git clone https://github.com/RiotTheClanker/ProtoFace.git
cd ProtoFace

# Build
pio run

# Flash via picotool (Pico must be plugged in normally)
pio run --target upload

# Open BLE serial monitor
pio device monitor
```

**Prebuilt firmware:** released versions (Mk2/Mk3 builds) are under
[Releases](https://github.com/RiotTheClanker/ProtoFace/releases) and on the website's flasher.
Every push and PR is built by the **Firmware build** workflow (`ProtoFace` under the run's
**Artifacts**); this development firmware is one file for every helmet.

### Releasing

1. Set `FIRMWARE_VERSION` in `src/main.cpp` to the new version (e.g. `"V6.2"`).
2. Add release notes as `docs/releases/<version>.md`.
3. Merge to `master`, then either push a tag with the same name
   (`git tag V6.2 && git push origin V6.2`), or open **Actions → Release → Run workflow**
   on `master` and enter the version. The tag is then created on the current `master` commit.

The **Release** workflow builds both models and publishes the release with
`ProtoFace-Mk2-<version>.uf2` and `ProtoFace-Mk3-<version>.uf2`. The names must keep
"Mk2"/"Mk3" in them, because that's how the website's flasher sorts files by board.

**Manual flash:** Hold **BOOTSEL** while plugging in USB → Pico mounts as a drive.  
Copy `.pio/build/rpipico2w/firmware.uf2` onto it.

---

## 📡 BLE Control

Connect to **"ProtoFace"** with any BLE UART app (e.g. [Serial Bluetooth Terminal](https://play.google.com/store/apps/details?id=de.kai_morich.serial_bluetooth_terminal) on Android, or **LightBlue** on iOS).

### Commands

| Command | Description |
|---|---|
| `help` | Show all commands |
| `list` | List `.anim` files on SD |
| `load <n>` | Load file by index |
| `fallback` | Switch to the built-in fallback face |
| `play` | Resume animation |
| `pause` | Pause animation |
| `next` | Advance one frame |
| `prev` | Go back one frame |
| `bright <n>` | Set brightness 0–255 (`bright` alone shows it) |
| `status` | Show version, display, file, frame, SD, brightness, power and mic level |
| `power` | Show USB-C PD details and the LED current budget |
| `reload` | Rescan SD card for new files; restarts if `display.json` changed |
| `ident` | Show each panel's position on its LED chain (toggle) |
| `display` | Show the display config in use |
| `display clear` | Remove the display config from flash and restart |

---

## 🎞️ Animation File Format (`.anim`)

Binary file format custom to this project:

```
Header, version 1 (8 bytes) — Mk2/Mk3 files:
  [0–3]  "ANIM"       magic
  [4]    version      uint8_t   — 1
  [5]    panels       uint8_t   — 11 or 14 panels of 8×8
  [6–7]  (reserved)
Header, version 2 (16 bytes) — any display:
  [0–3]  "ANIM"       magic
  [4]    version      uint8_t   — 2
  [5]    (reserved)
  [6–7]  header size  uint16_t  — 16
  [8–11] pixel count  uint32_t
  [12–15] layout ID   uint32_t  — CRC-32 of the panel sizes, see docs/display-config.md
Per frame:
  [0–1]  duration_ms  uint16_t  — little-endian. TIMED: hold time.
                                  SOUND: minimum hold before a trigger is accepted
  [2]    timing_mode  uint8_t   — 0=TIMED  1=SOUND
  [3]    (reserved)
  Then pixel count × 5 bytes of LED data, in the config's panel order:
    [0]  r           uint8_t
    [1]  g           uint8_t
    [2]  b           uint8_t
    [3]  sound_mode  uint8_t   — 0=STATIC  1=SNAP  2=LINEAR
    [4]  param       uint8_t   — SNAP: threshold  LINEAR: high nibble=m, low nibble=b
```

**SOUND frames** advance when the mic level rises past 128 (after at least `duration_ms`).
It has to drop below 96 again before the next SOUND frame can trigger, so one loud
sound advances one frame.

Place `.anim` files in the root of a FAT32-formatted SD card. A file named `start.anim`
is loaded automatically at boot; the others can be picked with `list` / `load <n>`.

### Microphone

`readVolume()` measures the **peak-to-peak** swing of the SPW2430's DC output on GP26
(which cancels its DC bias), subtracts a noise floor, applies a loudness curve and smooths
it with a fast-attack / slow-decay envelope. Tune `MIC_NOISE_FLOOR`, `MIC_FULLSCALE`,
`MIC_ATTACK` and `MIC_DECAY` in `main.cpp`; `status` shows the current level.

### Power

With a HUSB238 USB-C PD board on I2C0 (GP4/GP5), the firmware asks the charger what it can
supply at 5 V and budgets LED current to 85% of it, less 300 mA for the rest of the system.
Each frame's draw is estimated, and only frames that would exceed the budget are dimmed
(never below the config's `min_brightness`, 40/255 by default). Without the HUSB238 the
config's `max_ma` is used, or a 0.5 A USB supply is assumed if it has none.

---

## 📦 Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Purpose |
|---|---|
| `adafruit/Adafruit NeoPixel` | LED strip driver |
| `bblanchon/ArduinoJson` | Reads `display.json` |
| `LittleFS` | Flash copy of `display.json` (built into earlephilhower/arduino-pico) |
| `Adafruit_HUSB238` + `adafruit/Adafruit BusIO` | USB-C PD negotiation (HUSB238) |
| `BTstackLib` | Bluetooth LE stack (built into earlephilhower/arduino-pico) |
| `SD` | SD card file I/O (built into earlephilhower/arduino-pico) |

---

## 🤝 Contributing

1. Fork the repo
2. Create a branch: `git checkout -b feat/my-feature`
3. Commit: `git commit -m "feat: describe your change"`
4. Push & open a Pull Request

---

## 📄 License

MIT — see [LICENSE](LICENSE) for details.
