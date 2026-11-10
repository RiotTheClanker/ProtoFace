# Display config (`display.json`)

> Status: in development on the `dev` branch. The firmware drives `ws2812` outputs from it;
> `hub75` and `max7219` are checked but not driven yet.

A display config describes a face: what the artist draws (**panels**) and how those
pixels reach the hardware (**outputs**). The same file is used by the firmware and by the
[AnimFile Maker](https://github.com/RiotTheClanker/Protogen_AnimFile_Maker).

- Presets: [`displays/mk2.json`](../displays/mk2.json), [`displays/mk3.json`](../displays/mk3.json)
- Examples: [`displays/examples/`](../displays/examples/) (one or two plain HUB75 panels, MAX7219, custom NeoPixel PCB)
- JSON schema: [`displays/display.schema.json`](../displays/display.schema.json)

## Where the firmware gets its config

Put the file in the root of the SD card, named `display.json`. At boot the firmware:

1. Reads `/display.json` from the SD card, if there is a card and the file is there.
2. If it differs from the copy in flash, checks the whole file. A good file is saved to
   flash. A bad file is not used, the reason is shown by `status` / `display`, and the
   copy already in flash stays in use.
3. Uses the copy in flash.

So the SD card is only needed when the config changes: once a config is in flash, the
helmet keeps working (with the built-in "NO SD" face) without a card. To change the
display, replace `display.json` on the card and reboot or send `reload`; the Pico restarts
when the file changed.

A Pico without a config in flash lights nothing until a `display.json` is put on the card.

| Command | What it does |
|---|---|
| `display` | Show the active config: name, pixel count, panels, outputs |
| `display clear` | Delete the flash copy and restart (a `display.json` on the card is copied in again) |
| `reload` | Rescan the card; restarts if `display.json` changed |
| `ident` | Each panel shows its position on its output's chain, to check the wiring |

The file may be at most 48 KB.

## Pins

A config chooses the pins of its outputs. These GPIOs belong to ProtoFace itself and are
refused anywhere in a config, so a config can never take over the SD card or other
hardware:

| GPIO | Used for |
|---|---|
| GP4, GP5 | USB-C PD chip (I2C) |
| GP16–GP19 | SD card (SPI) |
| GP26 | Microphone |
| GP23–GP25, GP29 | Pico 2 W wireless chip |

Valid pins are GP0–GP28 except those. One pin can only be used once in a config.

## Writing a config

The config can be written by hand, by a tool, or exported by the AnimFile Maker. A
generator should produce a file that passes these checks, which the firmware and the
AnimFile Maker both apply (the schema in `displays/display.schema.json` covers the shape,
the rest needs code):

- `protoface_display` is `1` and `name` is not empty.
- Panel ids are unique; `w` and `h` are 1–256; `pixels` entries lie inside `w`×`h` and
  don't repeat.
- Every panel is listed by exactly one output, exactly once.
- Pins are valid, not reserved and not shared (see [Pins](#pins)).
- At most 4,096 pixels in total.
- A `hub75` placement fits inside the framebuffer; every `max7219` panel is 8×8.

## Top level

| Key | Required | Meaning |
|---|---|---|
| `protoface_display` | yes | Format version, `1` |
| `name` | yes | Short name shown by the firmware and the tool |
| `description` | no | Free text |
| `panels` | yes | Logical drawing surfaces, in `.anim` data order |
| `mirror` | no | How "Mirror to other side" pairs panels in the tool |
| `outlines` | no | `false` hides the region outlines and labels in the tool (default `true`) |
| `outputs` | yes | Hardware drivers and which panels each one shows |
| `limits` | no | Power-budget settings |

## Panels

A panel is a rectangle of pixels the artist paints. The `panels` array order is the order
the pixel data is stored in a `.anim` frame, so it must not be reordered once animations
exist for the config. Rewiring is done in `outputs`, never by reordering panels.

| Key | Required | Meaning |
|---|---|---|
| `id` | yes | Unique name, referenced by `outputs` |
| `w`, `h` | yes | Size in pixels (1–256) |
| `x`, `y` | yes | Top-left position on the painter canvas, in pixels. Only affects how the tool draws it |
| `side` | no | Which half of the visor, usually `left` or `right`. Used for mirroring and labels |
| `region` | no | Any name, usually `eye`, `mouth`, `nose`. The tool makes a fill button per region. Leave it out for a plain panel, such as one big HUB75 panel: it is filled and labelled by its `id` |
| `pixels` | no | Irregular panels: a list of `[x, y]` LED positions inside `w`×`h`, in LED order |

Pixel order inside a panel, which is both the `.anim` order and the order the firmware sees:

- Grid panel (no `pixels`): row by row from the top-left, `index = y * w + x`.
- Panel with `pixels`: the order of the list. Only those positions exist; the tool draws
  nothing in the other cells.

## Mirror

```json
"mirror": { "from": "left", "to": "right", "match": "region", "flip": "h" }
```

Painting a pixel on a `from`-side panel also paints the matching `to`-side panel. With
`"match": "region"` (the default) panels are matched by `region` and then by their order
within the region. With `"match": "order"` the two sides' panels are paired in order, which
suits panels without regions, such as one HUB75 panel per side. With `"flip": "h"` the image
is mirrored left↔right: pixels are flipped inside the panel and the panel order within each
region is reversed. `"flip": "none"` copies the image as-is. The tool's checkbox starts at
this value and the artist can change it.

## Outputs

Every panel must appear in exactly one output, exactly once. All outputs share these keys:

| Key | Meaning |
|---|---|
| `type` | `ws2812`, `hub75` or `max7219` |
| `name` | Label used by `ident` and in error messages |

### Panel transforms

Wherever an output lists a panel, the entry is either the panel's `id` or an object:

```json
{ "panel": "R_eye1", "flip": "h", "rotate": 90 }
```

`flip` (`none`, `h`, `v`, `hv`) mirrors the panel's image, then `rotate` (0, 90, 180, 270,
clockwise) turns it. These describe how the panel is mounted. A panel rotated by 90 or 270
occupies `h`×`w` on the hardware.

### `ws2812` (NeoPixel, WS2812B, SK6812 …)

```json
{ "type": "ws2812", "name": "left", "pin": 2, "color_order": "GRB",
  "wiring": "rows", "start": "top-left",
  "chain": ["L_eye1", "L_eye2", { "panel": "L_nose", "rotate": 180 }] }
```

| Key | Default | Meaning |
|---|---|---|
| `pin` | — | GPIO of the data line |
| `color_order` | `GRB` | `RGB`, `GRB`, `BGR`, … |
| `chain` | — | Panels in the order the data line reaches them |
| `wiring` | `rows` | LED order inside each grid panel: `rows`, `serpentine` (rows, every other row reversed), `columns`, `columns_serpentine` |
| `start` | `top-left` | Corner of the first LED: `top-left`, `top-right`, `bottom-left`, `bottom-right` |

`wiring` and `start` can also be set on a single chain entry. Panels with `pixels` ignore
them, because the list is already in LED order.

Mk2 and Mk3 use `rows` from `top-left`, which matches the 8×8 panels used so far.

### `hub75`

```json
{ "type": "hub75", "name": "visor", "width": 64, "height": 32, "chain": 2, "scan": 16,
  "pins": { "rgb": [6, 7, 8, 9, 10, 11], "addr": [12, 13, 14, 15], "clk": 20, "lat": 21, "oe": 22 },
  "place": [ { "panel": "L_face", "x": 0, "y": 0 }, { "panel": "R_face", "x": 64, "y": 0 } ] }
```

| Key | Meaning |
|---|---|
| `width`, `height` | Size of one HUB75 panel in pixels |
| `chain` | Number of panels daisy-chained left to right; the framebuffer is `width × chain` by `height` |
| `scan` | Rows driven at once: 8, 16 or 32 (1/8, 1/16, 1/32 scan) |
| `pins` | `rgb` = R1 G1 B1 R2 G2 B2, `addr` = A B C D (E), plus `clk`, `lat`, `oe` |
| `place` | Where each panel sits in the framebuffer: `{ "panel", "x", "y" }` plus optional `flip`/`rotate` |

Framebuffer pixels no panel covers stay black.

### `max7219`

```json
{ "type": "max7219", "name": "matrix", "pins": { "din": 7, "clk": 6, "cs": 8 },
  "threshold": 64, "intensity": 8, "chain": ["eye1", "eye2", "mouth1"] }
```

Single-colour 8×8 modules on one SPI chain. Every panel on a MAX7219 output must be 8×8
(after rotation). A pixel is on when `max(r, g, b)` after sound reaction and brightness is at
least `threshold` (0–255, default 64). `intensity` is the module brightness (0–15). `chain`
lists panels starting with the module nearest the Pico.

## Limits

| Key | Default | Meaning |
|---|---|---|
| `ma_per_led_white` | 15 | Current one RGB LED draws at full white |
| `ma_per_led_idle` | 1 | Current one LED draws when off |
| `min_brightness` | 40 | Power limiting never dims below this (0–255) |
| `max_ma` | — | Hard cap for LED current, used when there is no USB-PD board |

## `.anim` files

Frames keep the same layout: a 4-byte frame header and 5 bytes per pixel (R, G, B, sound
mode, parameter), with pixels in panel order. Only the file header changes.

```
Version 2 header (16 bytes):
  [0–3]   "ANIM"
  [4]     version      = 2
  [5]     reserved     = 0
  [6–7]   header size  = 16      uint16 LE
  [8–11]  pixel count            uint32 LE
  [12–15] layout ID              uint32 LE
```

The **layout ID** is a CRC-32 (the zlib / PNG one) over, for each panel in order:
`w` (uint16 LE), `h` (uint16 LE), pixel count (uint16 LE), then for `pixels` panels each
position as two bytes `x, y`. Panel ids, positions on the canvas, regions and outputs are
not part of it, so renaming a panel or rewiring a helmet never invalidates animations.

The firmware plays a file only if its pixel count and layout ID match the active config.

Version 1 files (8-byte header, byte 5 = 11 or 14) keep working: they load on a config made
of exactly that many 8×8 grid panels, which is true for the Mk2 and Mk3 presets.
The AnimFile Maker still saves version 1 for such displays, so released firmware keeps
playing files made for Mk2 and Mk3.

## Limits of the first firmware version

- Only `ws2812` outputs light up. A config with `hub75` or `max7219` outputs is accepted
  but the firmware reports "not supported by this firmware yet" and shows nothing.
- One output type at a time (all `ws2812`, or one `hub75`, or one `max7219`).
- At most 4,096 pixels in total.
- At most 4 `ws2812` outputs.
