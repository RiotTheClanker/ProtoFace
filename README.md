# 🐾 ProtoFace — Protogen Visor Firmware

Firmware for a Protogen suit visor running on the **Raspberry Pi Pico 2 W**.  
Drives two chains of **448 NeoPixels each (896 total)**, reads animations from an SD card,
reacts to a microphone, and is controlled wirelessly via **Bluetooth LE (NUS/UART profile)**.

![Build Status](https://github.com/RiotTheClanker/ProtoFace/actions/workflows/build.yml/badge.svg)

---

## ✨ Features

| Feature | Details |
|---|---|
| 🌈 **Dual LED chains** | 2 strips (GP2 left, GP3 right) via Adafruit NeoPixel |
| 💾 **SD card animations** | Plays `.anim` files from SD — hot-swappable at runtime |
| 🎤 **Sound reactivity** | Three modes per LED: Static, Snap (threshold), Linear (volume-scaled) |
| 📡 **Bluetooth LE control** | NUS UART profile — connect with any BLE serial app |
| 🔁 **Fallback animation** | Built-in warm orange glow if SD is absent or files are missing |
| ⌨️ **CLI over BLE** | Full command set: `list`, `load`, `play`, `pause`, `next`, `prev`, `status`, `reload` |

---

## 🗂️ Repository Structure

```
ProtoFace/
├── src/
│   ├── main.cpp            # Main firmware — setup, loop, BLE, SD, animation engine
│   └── fallback_anim.h     # Built-in fallback frame (compiled into flash)
├── .github/
│   └── workflows/
│       └── build.yml       # GitHub Actions CI — builds and uploads firmware.uf2
├── .gitignore              # Excludes .pio/ build cache and binaries
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

> SPI (SD card) uses the Pico's default SPI0 pins: SCK=GP18, MOSI=GP19, MISO=GP16.

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
| `play` | Resume animation |
| `pause` | Pause animation |
| `next` | Advance one frame |
| `prev` | Go back one frame |
| `status` | Show current state (file, frame, SD status) |
| `reload` | Rescan SD card for new files |

---

## 🎞️ Animation File Format (`.anim`)

Binary file format custom to this project:

```
Header  (8 bytes):  "ANIM" magic + reserved
Per frame:
  [0–1]  duration_ms  uint16_t  — frame hold time in ms (0 = sound-triggered)
  [2]    timing_mode  uint8_t   — 0=TIMED  1=SOUND
  [3]    (reserved)
  Then 896 × 5 bytes of LED data:
    [0]  r           uint8_t
    [1]  g           uint8_t
    [2]  b           uint8_t
    [3]  sound_mode  uint8_t   — 0=STATIC  1=SNAP  2=LINEAR
    [4]  param       uint8_t   — SNAP: threshold  LINEAR: high nibble=m, low nibble=b
```

Place `.anim` files in the root of a FAT32-formatted SD card. They are auto-loaded on boot.

---

## 📦 Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Purpose |
|---|---|
| `adafruit/Adafruit NeoPixel` | LED strip driver |
| `BTstackLib` | Bluetooth LE stack (earlephilhower/arduino-pico) |
| `SD` | SD card file I/O |

---

## 🤝 Contributing

1. Fork the repo
2. Create a branch: `git checkout -b feat/my-feature`
3. Commit: `git commit -m "feat: describe your change"`
4. Push & open a Pull Request

---

## 📄 License

MIT — see [LICENSE](LICENSE) for details.
