// pf_config.h — display configs (display.json) for ProtoFace
//
// Portable C++ (no Arduino calls), so it is unit-tested on a PC as well
// (tests/host).  The format is documented in docs/display-config.md and must
// stay in step with display_config.py in the AnimFile Maker.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace pf {

static const int      FORMAT_VERSION = 1;
static const uint32_t MAX_PIXELS     = 4096;
static const int      MAX_WS2812     = 4;      // NeoPixel outputs per helmet

enum OutputType : uint8_t { OUT_WS2812, OUT_HUB75, OUT_MAX7219 };
enum Wiring     : uint8_t { WIRE_ROWS, WIRE_SERPENTINE, WIRE_COLUMNS, WIRE_COLUMNS_SERPENTINE };
enum Corner     : uint8_t { START_TOP_LEFT, START_TOP_RIGHT, START_BOTTOM_LEFT, START_BOTTOM_RIGHT };
enum Flip       : uint8_t { FLIP_NONE = 0, FLIP_H = 1, FLIP_V = 2, FLIP_HV = 3 };

struct Point { uint8_t x, y; };

struct Panel {
    std::string id, side, region;     // side/region may be empty
    uint16_t w = 0, h = 0;            // size in pixels
    int32_t  x = 0, y = 0;            // position on the painter canvas
    bool     custom = false;          // has an explicit "pixels" list
    std::vector<Point> pixels;        // custom panels only, in LED / .anim order
    uint32_t offset = 0;              // index of the first pixel in a frame
    uint32_t count() const { return custom ? (uint32_t)pixels.size() : (uint32_t)w * h; }
    // (x, y) of pixel i inside the panel
    Point at(uint32_t i) const;
    // index of the pixel at (x, y) inside the panel, or -1
    int32_t indexOf(int x, int y) const;
};

// One panel as an output shows it.
struct Placement {
    uint16_t panel = 0;               // index into DisplayConfig::panels
    uint8_t  flip = FLIP_NONE;
    uint16_t rotate = 0;              // 0, 90, 180, 270 (clockwise)
    uint8_t  wiring = WIRE_ROWS;
    uint8_t  start = START_TOP_LEFT;
    uint16_t x = 0, y = 0;            // HUB75 framebuffer position
};

struct Output {
    OutputType  type = OUT_WS2812;
    std::string name;
    // ws2812
    uint8_t     pin = 0;
    std::string colorOrder = "GRB";
    // hub75
    uint16_t    width = 0, height = 0, chain = 1, scan = 16;
    std::vector<uint8_t> rgbPins, addrPins;
    uint8_t     clkPin = 0, latPin = 0, oePin = 0;
    // max7219
    uint8_t     dinPin = 0, spiClkPin = 0, csPin = 0, threshold = 64, intensity = 8;

    std::vector<Placement> place;     // chain order (ws2812, max7219) or placements (hub75)
    uint32_t    length = 0;           // physical LEDs / framebuffer pixels
    // For every physical LED / framebuffer pixel: the frame pixel it shows, or -1.
    std::vector<int32_t> source;
};

struct Mirror {
    bool        enabled = false;
    std::string from, to;
    bool        byOrder = false;      // "match": "order" (else by region)
    bool        flipH = false;
};

struct Limits {
    float   maPerLedWhite = 15.0f;
    float   maPerLedIdle  = 1.0f;
    uint8_t minBrightness = 40;
    float   maxMA         = 0.0f;     // 0 = no cap set
};

struct DisplayConfig {
    std::string name, description;
    std::vector<Panel>  panels;
    std::vector<Output> outputs;
    Mirror   mirror;
    Limits   limits;
    uint32_t totalPixels = 0;

    uint32_t layoutId() const;
    // A version-1 .anim (11 or 14 panels of 8x8) fits this display.
    bool v1Compatible(int panelCount) const;
    // {frame index -> frame index on the other side}, -1 where there is none.
    std::vector<int32_t> mirrorMap() const;
    int panelIndex(const std::string &id) const;
};

// GPIOs the firmware uses for itself: SD card, microphone, USB-PD I2C and
// the Pico 2 W's wireless chip.  A display.json may not use them.
bool isReservedPin(int gpio);
const char *reservedPinList();

// Parse and check a display.json.  On failure returns false and puts a
// one-line explanation in `error`.
bool parseConfig(const char *json, size_t len, DisplayConfig &out, std::string &error);

// CRC-32 (zlib / PNG polynomial), used for the layout ID and uploads.
uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc = 0);

// Physical position (0..) of a panel pixel on an output, for ws2812 / max7219
// chains: `base` is the index of the panel's first LED on the chain.
uint32_t chainIndex(const Panel &p, const Placement &pl, uint32_t pixel);

// Built-in "NO SD" face drawn from the config's regions: fills rgb (3 bytes
// per frame pixel, already sized totalPixels * 3).
void drawNoSdFace(const DisplayConfig &cfg, uint8_t *rgb);

}  // namespace pf
