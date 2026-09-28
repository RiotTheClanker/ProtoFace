# 🐾 ProtoFace — Protogen Visor Firmware

Firmware for a Protogen suit visor running on the **Raspberry Pi Pico 2 W**.  
Drives two NeoPixel chains (one per side of the visor), reads animations from an SD card,
reacts to a microphone, and is controlled wirelessly via **Bluetooth LE (NUS/UART profile)**.

Animations are made with the companion tool,
[Protogen AnimFile Maker](https://github.com/RiotTheClanker/Protogen_AnimFile_Maker).

---

## ✨ Features

| Feature | Details |
|---|---|
| 🌈 **Dual LED chains** | 2 strips (GP2 left, GP3 right) via Adafruit NeoPixel |
| 🧩 **Two panel layouts** | 11-panel (6+5, default) or 14-panel (7+7), chosen at build time |
| 💾 **SD card animations** | Plays `.anim` files from SD — `start.anim` auto-loads at boot |
| 🎤 **Sound reactivity** | Three modes per LED: Static, Snap (threshold), Linear (volume-scaled), plus sound-triggered frames |
| 📡 **Bluetooth LE control** | NUS UART profile — connect with any BLE serial app |
| 🔁 **Fallback animation** | Built-in frame shown when there is no SD card or `start.anim` |
| ⌨️ **CLI over BLE** | `list`, `load`, `fallback`, `play`, `pause`, `next`, `prev`, `bright`, `status`, `reload` |

---

## 🗂️ Repository Structure

```
ProtoFace/
├── src/
│   ├── main.cpp            # Main firmware — setup, loop, BLE, SD, animation engine
│   └── fallback_anim.h     # (optional, not committed) fallback frame exported by the tool
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
| Microphone (analog) | GP26 | ADC0 — analog sound level |

> SPI (SD card) uses SPI0: SCK=GP18, MOSI=GP19, MISO=GP16 (set explicitly in `setup()`).

---

## 🧩 Panel Layouts

Each panel is 8×8 = 64 LEDs. The first side is wired to the left chain (GP2), the second side to the right chain (GP3).

| Layout | Left chain (GP2) | Right chain (GP3) | Total |
|---|---|---|---|
| **11** (default) | Nose side: eyes 0–1, mouth 2–4, nose 5 — 384 LEDs | Plain side: eyes 6–7, mouth 8–10 — 320 LEDs | 704 |
| **14** | Eyes 0–1, mouth 2–5, nose 6 — 448 LEDs | Eyes 7–8, mouth 9–12, nose 13 — 448 LEDs | 896 |

Select the layout with `PROTOGEN_LAYOUT` (default `11`), either in `src/main.cpp` or by
uncommenting `-DPROTOGEN_LAYOUT=14` in `platformio.ini`. `.anim` files store their layout
in the header, and the firmware refuses to play a file made for the other layout.

---

## 🔁 Fallback Animation

The fallback frame is shown at boot when there is no SD card or no `start.anim`, and
whenever a file fails to load.

- **Default:** with no `src/fallback_anim.h`, a dim warm-orange frame is used, so a fresh
  clone builds as-is.
- **Custom:** in the AnimFile Maker, open the **Export .h** tab, pick a frame and export
  `fallback_anim.h` into `src/`, then rebuild. The tool's layout selector must match
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
| `status` | Show current state (file, frame, SD, layout, brightness, mic level) |
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

`readVolume()` measures the **peak-to-peak** swing on GP26, which suits raw analog mic
modules whose output idles at mid-rail (MAX4466, MAX9814, …). If your module outputs an
envelope instead, set `MIC_PEAK_TO_PEAK` to `0` in `main.cpp`. Use `status` to see the
current level.

---

## 📦 Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Purpose |
|---|---|
| `adafruit/Adafruit NeoPixel` | LED strip driver |
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
