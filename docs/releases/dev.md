## ProtoFace dev test build

> ⚠️ **Test build from the `dev` branch. Not tested on a helmet yet.** Keep the last
> stable release (V6.2) handy so you can flash back.

One firmware for every helmet: **`ProtoFace-<version>.uf2`**. There are no more separate
Mk2 / Mk3 files. Which display it drives comes from a `display.json` file on the SD card.

### Setting up the SD card
1. Download the `.json` that matches your display from the files below:
   - `mk2.json`: Mk2 helmet (6 + 5 panels)
   - `mk3.json`: Mk3 helmet (7 + 7 panels)
   - `hub75-1x64x32.json` / `hub75-2x64x32.json`: one or two 64x32 HUB75 panels
   - `max7219-single-side.json`, `neopixel-custom-eye.json`: other examples to adapt
2. Rename it to **`display.json`** and copy it to the **root** of the SD card, next to your `.anim` files.
3. Flash the `.uf2` (hold BOOTSEL, plug in, drag the file onto the RPI-RP2 drive), put the card in, and power on.

At boot the firmware checks `display.json` and copies it into the Pico's flash, so the
helmet keeps its display setup even without the card. To change displays, replace the file
on the card and reboot (or send `reload`). Without a `display.json` the Pico lights nothing.
A bad file is rejected and the reason is shown by the `status` / `display` Bluetooth commands.

`display.schema.json` describes the file format, and
[docs/display-config.md](https://github.com/RiotTheClanker/ProtoFace/blob/dev/docs/display-config.md) explains every field.

### Animations
- Existing Mk2 / Mk3 `.anim` files still play with `mk2.json` / `mk3.json`.
- Other displays need `.anim` files from the AnimFile Maker dev build, made with the same display config.

### Please report
Whether the face shows on the right panels, how the brightness looks, and anything in
`status` / `display` that looks wrong.
