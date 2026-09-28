# 🐾 ProtoFace — Protogen Visor Firmware

Firmware for a Protogen suit visor running on the **Raspberry Pi Pico 2 W**.  
Drives two NeoPixel chains (one per side of the visor), reads animations from an SD card,
reacts to a microphone, and is controlled wirelessly via **Bluetooth LE (NUS/UART profile)**.

[![Firmware build](https://github.com/RiotTheClanker/ProtoFace/actions/workflows/build.yml/badge.svg)](https://github.com/RiotTheClanker/ProtoFace/actions/workflows/build.yml)

Animations are made with the companion tool,
[Protogen AnimFile Maker](https://github.com/RiotTheClanker/Protogen_AnimFile_Maker).

---

## ✨ Features

| Feature | Details |
|---|---|
| 🌈 **Dual LED chains** | 2 strips (GP2 left, GP3 right) via Adafruit NeoPixel |
| 🧩 **Mk2 & Mk3 helmets** | Mk2 = 11 panels (6+5), Mk3 = 14 panels (7+7), chosen at build time |
| 💾 **SD card animations** | Plays `.anim` files from SD — `start.anim` auto-loads at boot |
| 🎤 **Sound reactivity** | Three modes per LED: Static, Snap (threshold), Linear (volume-scaled), plus sound-triggered frames |
| 📡 **Bluetooth LE control** | NUS UART profile — connect with any BLE serial app |
| ⚡ **USB-C power budgeting** | HUSB238 PD negotiation + per-frame dimming to stay within the supply |
| 🔁 **Fallback animation** | Built-in frame per model, shown when there is no SD card or `start.anim` |
| ⌨️ **CLI over BLE** | `list`, `load`, `fallback`, `play`, `pause`, `next`, `prev`, `bright`, `status`, `power`, `reload` |

---

## 🗂️ Repository Structure

```
ProtoFace/
├── src/
│   ├── main.cpp            # Main firmware — setup, loop, BLE, SD, power, animation engine
│   ├── fallback/
│   │   ├── mk2_fallback.h  # Mk2 built-in fallback frame (layout 11)
│   │   └── mk3_fallback.h  # Mk3 built-in fallback frame (layout 14)
│   └── fallback_anim.h     # (optional, gitignored) local override exported by the tool
├── .github/workflows/
│   ├── build.yml           # CI — builds Mk2 + Mk3 on every PR / push
│   └── release.yml         # On a V* tag — builds both and publishes the release
├── docs/releases/          # Release notes, one file per version tag
├── .gitignore              # Excludes .pio/ build cache, binaries and fallback_anim.h
├── platformio.ini          # PlatformIO project config
└── README.md
```

---

## 🔧 Hardware Pinout

| Signal | GPIO | Notes |
|---|---|---|
| Left LED chain | GP2 | NeoPixel data |
| Right LED chain | GP3 | NeoPixel data |
| SD card CS | GP17 | SPI chip-select |
| Microphone (analog) | GP26 | ADC0 — SPW2430 DC output |
| USB-C PD (HUSB238) SDA | GP4 | I2C0 — optional; without it a 0.5 A USB budget is assumed |
| USB-C PD (HUSB238) SCL | GP5 | I2C0 |

> SPI (SD card) uses SPI0: SCK=GP18, MOSI=GP19, MISO=GP16 (set explicitly in `setup()`).

---

## 🧩 Models & Panel Layouts

Each panel is 8×8 = 64 LEDs. The first side is wired to the left chain (GP2), the second side to the right chain (GP3).

| Model | Layout | Left chain (GP2) | Right chain (GP3) | Total |
|---|---|---|---|---|
| **Mk2** (default) | 11 | Nose side: eyes 0–1, mouth 2–4, nose 5 — 384 LEDs | Plain side: eyes 6–7, mouth 8–10 — 320 LEDs | 704 |
| **Mk3** | 14 | Eyes 0–1, mouth 2–5, nose 6 — 448 LEDs | Eyes 7–8, mouth 9–12, nose 13 — 448 LEDs | 896 |

Select the model with `PROTOGEN_LAYOUT` (default `11` = Mk2), either in `src/main.cpp` or by
uncommenting `-DPROTOGEN_LAYOUT=14` in `platformio.ini`. `.anim` files store their layout
in the header, and the firmware refuses to play a file made for the other layout.

LED data is stored in logical face order (eye → mouth → nose) on both sides. The left
chain's physical wiring runs the other way (nose → mouth → eye), so `pushFrame()` writes
the left chain's panels in reverse order; the right chain is written as-is.

---

## 🔁 Fallback Animation

The fallback frame is shown at boot when there is no SD card or no `start.anim`, and
whenever a file fails to load.

- **Built in:** each model has its own frame in `src/fallback/` (`mk2_fallback.h`,
  `mk3_fallback.h`), and the build picks the one for the selected model.
- **Change it:** in the AnimFile Maker, open the **Export .h** tab, pick a frame and export it
  over the model's file in `src/fallback/` (commit it to ship it in releases), or save it as
  `src/fallback_anim.h` for a local-only override. The tool's layout selector must match
  `PROTOGEN_LAYOUT`; if it doesn't, the build stops with an error that says so.

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

**Prebuilt firmware:** download `ProtoFace-Mk2-<version>.uf2` or `ProtoFace-Mk3-<version>.uf2`
from [Releases](https://github.com/RiotTheClanker/ProtoFace/releases), or use the flasher on the
website. Every push and PR is also built for both models by the **Firmware build** workflow
(`ProtoFace-Mk2` / `ProtoFace-Mk3` under the run's **Artifacts**).

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
| `fallback` | Switch to the built-in fallback frame |
| `play` | Resume animation |
| `pause` | Pause animation |
| `next` | Advance one frame |
| `prev` | Go back one frame |
| `bright <n>` | Set brightness 0–255 (`bright` alone shows it) |
| `status` | Show version, model, file, frame, SD, brightness, power and mic level |
| `power` | Show USB-C PD details and the LED current budget |
| `reload` | Rescan SD card for new files |

---

## 🎞️ Animation File Format (`.anim`)

Binary file format custom to this project:

```
Header  (8 bytes):
  [0–3]  "ANIM"       magic
  [4]    version      uint8_t   — 1
  [5]    panels       uint8_t   — layout: 11 or 14 (must match PROTOGEN_LAYOUT)
  [6–7]  (reserved)
Per frame:
  [0–1]  duration_ms  uint16_t  — little-endian. TIMED: hold time.
                                  SOUND: minimum hold before a trigger is accepted
  [2]    timing_mode  uint8_t   — 0=TIMED  1=SOUND
  [3]    (reserved)
  Then (panels × 64) × 5 bytes of LED data (704 for layout 11, 896 for layout 14),
  in chain order — left chain first, then right chain:
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
(never below 40/255). Without the HUSB238 a 0.5 A USB supply is assumed.

---

## 📦 Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Purpose |
|---|---|
| `adafruit/Adafruit NeoPixel` | LED strip driver |
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
