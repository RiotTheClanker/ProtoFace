// pf_config.cpp — see pf_config.h
#include "pf_config.h"

#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>
#include <set>
#include <map>

namespace pf {

// ── Panels ────────────────────────────────────────────────────────────────────
Point Panel::at(uint32_t i) const {
    if (custom) return pixels[i];
    return Point{(uint8_t)(i % w), (uint8_t)(i / w)};
}

int32_t Panel::indexOf(int px, int py) const {
    if (px < 0 || py < 0 || px >= w || py >= h) return -1;
    if (!custom) return py * w + px;
    for (size_t i = 0; i < pixels.size(); i++)
        if (pixels[i].x == px && pixels[i].y == py) return (int32_t)i;
    return -1;
}

int DisplayConfig::panelIndex(const std::string &id) const {
    for (size_t i = 0; i < panels.size(); i++)
        if (panels[i].id == id) return (int)i;
    return -1;
}

// ── CRC-32 / layout ID ────────────────────────────────────────────────────────
uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t DisplayConfig::layoutId() const {
    // Same bytes as display_config.layout_id() in the AnimFile Maker:
    // per panel w, h, pixel count (uint16 LE), then custom pixel positions.
    uint32_t crc = 0;
    for (const Panel &p : panels) {
        uint32_t n = p.count();
        uint8_t hdr[6] = {(uint8_t)p.w, (uint8_t)(p.w >> 8), (uint8_t)p.h, (uint8_t)(p.h >> 8),
                          (uint8_t)n, (uint8_t)(n >> 8)};
        crc = crc32(hdr, 6, crc);
        if (p.custom)
            for (const Point &pt : p.pixels) {
                uint8_t xy[2] = {pt.x, pt.y};
                crc = crc32(xy, 2, crc);
            }
    }
    return crc;
}

bool DisplayConfig::v1Compatible(int panelCount) const {
    if ((int)panels.size() != panelCount) return false;
    for (const Panel &p : panels)
        if (p.custom || p.w != 8 || p.h != 8) return false;
    return true;
}

// ── Mirror map ────────────────────────────────────────────────────────────────
std::vector<int32_t> DisplayConfig::mirrorMap() const {
    std::vector<int32_t> out(totalPixels, -1);
    if (!mirror.enabled) return out;

    // Group each side's panels: by region (in panel order), or all in one group.
    auto groups = [&](const std::string &side) {
        std::vector<std::pair<std::string, std::vector<int>>> g;
        for (size_t i = 0; i < panels.size(); i++) {
            const Panel &p = panels[i];
            if (p.side != side) continue;
            if (!mirror.byOrder && p.region.empty()) continue;
            std::string key = mirror.byOrder ? std::string() : p.region;
            size_t k = 0;
            while (k < g.size() && g[k].first != key) k++;
            if (k == g.size()) g.push_back({key, {}});
            g[k].second.push_back((int)i);
        }
        return g;
    };
    auto src = groups(mirror.from);
    auto dst = groups(mirror.to);
    for (auto &sg : src) {
        const std::vector<int> *dp = nullptr;
        for (auto &dg : dst) if (dg.first == sg.first) dp = &dg.second;
        if (!dp) continue;
        std::vector<int> d = *dp;
        if (mirror.flipH) d.assign(dp->rbegin(), dp->rend());
        for (size_t k = 0; k < sg.second.size() && k < d.size(); k++) {
            const Panel &a = panels[sg.second[k]];
            const Panel &b = panels[d[k]];
            for (uint32_t i = 0; i < a.count(); i++) {
                Point pt = a.at(i);
                int tx = mirror.flipH ? (int)b.w - 1 - pt.x : pt.x;
                int32_t j = b.indexOf(tx, pt.y);
                if (j < 0) continue;
                out[a.offset + i] = (int32_t)(b.offset + j);
                out[b.offset + j] = (int32_t)(a.offset + i);
            }
        }
    }
    return out;
}

// ── Reserved pins ─────────────────────────────────────────────────────────────
// Keep in step with the pin #defines in main.cpp.
static const uint8_t RESERVED[] = {4, 5, 16, 17, 18, 19, 23, 24, 25, 26, 29};

bool isReservedPin(int gpio) {
    for (uint8_t r : RESERVED) if (r == gpio) return true;
    return false;
}

const char *reservedPinList() {
    return "GP4-5 (USB-PD), GP16-19 (SD card), GP26 (mic), GP23-25 and GP29 (wireless chip)";
}

// ── Pixel transforms ──────────────────────────────────────────────────────────
// Panel pixel (x, y) after the mounting flip and clockwise rotation; also
// returns the mounted size.
static void mount(const Panel &p, const Placement &pl, int x, int y,
                  int &ox, int &oy, int &mw, int &mh) {
    int w = p.w, h = p.h;
    if (pl.flip & FLIP_H) x = w - 1 - x;
    if (pl.flip & FLIP_V) y = h - 1 - y;
    switch (pl.rotate) {
        case 90:  ox = h - 1 - y; oy = x;         mw = h; mh = w; break;
        case 180: ox = w - 1 - x; oy = h - 1 - y; mw = w; mh = h; break;
        case 270: ox = y;         oy = w - 1 - x; mw = h; mh = w; break;
        default:  ox = x;         oy = y;         mw = w; mh = h; break;
    }
}

uint32_t chainIndex(const Panel &p, const Placement &pl, uint32_t pixel) {
    if (p.custom) return pixel;                 // the list is already in LED order
    Point pt = p.at(pixel);
    int x, y, w, h;
    mount(p, pl, pt.x, pt.y, x, y, w, h);
    if (pl.start == START_TOP_RIGHT || pl.start == START_BOTTOM_RIGHT) x = w - 1 - x;
    if (pl.start == START_BOTTOM_LEFT || pl.start == START_BOTTOM_RIGHT) y = h - 1 - y;
    switch (pl.wiring) {
        case WIRE_SERPENTINE:         if (y & 1) x = w - 1 - x; return (uint32_t)(y * w + x);
        case WIRE_COLUMNS:            return (uint32_t)(x * h + y);
        case WIRE_COLUMNS_SERPENTINE: if (x & 1) y = h - 1 - y; return (uint32_t)(x * h + y);
        default:                      return (uint32_t)(y * w + x);
    }
}

// ── Parsing ───────────────────────────────────────────────────────────────────
namespace {

struct Fail {
    std::string &err;
    bool operator()(const std::string &msg) { err = msg; return false; }
};

bool isInt(JsonVariantConst v) { return v.is<long>() && !v.is<bool>(); }

long constrainByte(long v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

bool parseEnum(JsonVariantConst v, const char *const *names, int n, int dflt, int &out) {
    if (v.isNull()) { out = dflt; return true; }
    if (!v.is<const char *>()) return false;
    for (int i = 0; i < n; i++)
        if (strcmp(v.as<const char *>(), names[i]) == 0) { out = i; return true; }
    return false;
}

const char *const WIRING_NAMES[] = {"rows", "serpentine", "columns", "columns_serpentine"};
const char *const START_NAMES[]  = {"top-left", "top-right", "bottom-left", "bottom-right"};
const char *const FLIP_NAMES[]   = {"none", "h", "v", "hv"};

std::string q(const std::string &s) { return "\"" + s + "\""; }

}  // namespace

bool parseConfig(const char *json, size_t len, DisplayConfig &out, std::string &error) {
    Fail fail{error};
    out = DisplayConfig();

    JsonDocument doc;
    DeserializationError jerr = deserializeJson(doc, json, len);
    if (jerr) return fail(std::string("not valid JSON: ") + jerr.c_str());
    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull()) return fail("the file must contain a JSON object");

    if (!isInt(root["protoface_display"]) || root["protoface_display"].as<int>() != FORMAT_VERSION)
        return fail("\"protoface_display\" must be 1");
    if (!root["name"].is<const char *>()) return fail("\"name\" must be a string of 1-32 characters");
    out.name = root["name"].as<const char *>();
    if (out.name.empty() || out.name.size() > 32) return fail("\"name\" must be a string of 1-32 characters");
    if (root["description"].is<const char *>()) out.description = root["description"].as<const char *>();

    // Panels
    JsonArrayConst panels = root["panels"].as<JsonArrayConst>();
    if (panels.isNull() || panels.size() == 0) return fail("\"panels\" must be a non-empty list");
    uint32_t total = 0;
    for (JsonVariantConst pv : panels) {
        JsonObjectConst po = pv.as<JsonObjectConst>();
        if (po.isNull()) return fail("each panel must be an object");
        if (!po["id"].is<const char *>() || !*po["id"].as<const char *>())
            return fail("a panel is missing \"id\"");
        Panel p;
        p.id = po["id"].as<const char *>();
        std::string where = "panel " + q(p.id);
        if (out.panelIndex(p.id) >= 0) return fail(where + ": id used twice");
        if (!isInt(po["w"]) || po["w"].as<long>() < 1 || po["w"].as<long>() > 256)
            return fail(where + ": \"w\" must be 1-256");
        if (!isInt(po["h"]) || po["h"].as<long>() < 1 || po["h"].as<long>() > 256)
            return fail(where + ": \"h\" must be 1-256");
        if (!isInt(po["x"]) || po["x"].as<long>() < 0) return fail(where + ": \"x\" must be 0 or more");
        if (!isInt(po["y"]) || po["y"].as<long>() < 0) return fail(where + ": \"y\" must be 0 or more");
        p.w = po["w"].as<uint16_t>();
        p.h = po["h"].as<uint16_t>();
        p.x = po["x"].as<int32_t>();
        p.y = po["y"].as<int32_t>();
        if (po["side"].is<const char *>())   p.side   = po["side"].as<const char *>();
        if (po["region"].is<const char *>()) p.region = po["region"].as<const char *>();
        if (!po["pixels"].isNull()) {
            JsonArrayConst px = po["pixels"].as<JsonArrayConst>();
            if (px.isNull() || px.size() == 0) return fail(where + ": \"pixels\" must be a non-empty list");
            if (px.size() > MAX_PIXELS) return fail(where + ": too many pixels");
            p.custom = true;
            std::set<uint32_t> seen;
            for (JsonVariantConst xy : px) {
                JsonArrayConst a = xy.as<JsonArrayConst>();
                if (a.isNull() || a.size() != 2 || !isInt(a[0]) || !isInt(a[1]))
                    return fail(where + ": each pixel must be [x, y]");
                long x = a[0].as<long>(), y = a[1].as<long>();
                char pos[32];
                snprintf(pos, sizeof pos, "[%ld, %ld]", x, y);
                if (x < 0 || y < 0 || x >= p.w || y >= p.h)
                    return fail(where + ": pixel " + pos + " is outside its area");
                if (!seen.insert((uint32_t)(y * 256 + x)).second)
                    return fail(where + ": pixel " + pos + " listed twice");
                p.pixels.push_back(Point{(uint8_t)x, (uint8_t)y});
            }
        }
        p.offset = total;
        total += p.count();
        if (total > MAX_PIXELS) {
            char msg[80];
            snprintf(msg, sizeof msg, "more than %lu pixels in total", (unsigned long)MAX_PIXELS);
            return fail(msg);
        }
        out.panels.push_back(p);
    }
    out.totalPixels = total;

    // Mirror
    JsonVariantConst mv = root["mirror"];
    if (!mv.isNull()) {
        JsonObjectConst m = mv.as<JsonObjectConst>();
        if (m.isNull() || !m["from"].is<const char *>() || !m["to"].is<const char *>())
            return fail("\"mirror\" needs \"from\" and \"to\" side names");
        out.mirror.enabled = true;
        out.mirror.from = m["from"].as<const char *>();
        out.mirror.to   = m["to"].as<const char *>();
        const char *const FL[] = {"none", "h"};
        const char *const MA[] = {"region", "order"};
        int f, ma;
        if (!parseEnum(m["flip"], FL, 2, 0, f)) return fail("mirror \"flip\" must be \"none\" or \"h\"");
        if (!parseEnum(m["match"], MA, 2, 0, ma)) return fail("mirror \"match\" must be \"region\" or \"order\"");
        out.mirror.flipH = f == 1;
        out.mirror.byOrder = ma == 1;
        for (const std::string *s : {&out.mirror.from, &out.mirror.to}) {
            bool found = false;
            for (const Panel &p : out.panels) found |= p.side == *s;
            if (!found) return fail("mirror: no panel has side " + q(*s));
        }
    }
    if (!root["outlines"].isNull() && !root["outlines"].is<bool>())
        return fail("\"outlines\" must be true or false");

    // Limits
    JsonObjectConst lim = root["limits"].as<JsonObjectConst>();
    if (!lim.isNull()) {
        if (lim["ma_per_led_white"].is<float>()) out.limits.maPerLedWhite = lim["ma_per_led_white"].as<float>();
        if (lim["ma_per_led_idle"].is<float>())  out.limits.maPerLedIdle  = lim["ma_per_led_idle"].as<float>();
        if (isInt(lim["min_brightness"]))        out.limits.minBrightness = (uint8_t)constrainByte(lim["min_brightness"].as<long>());
        if (lim["max_ma"].is<float>())           out.limits.maxMA         = lim["max_ma"].as<float>();
    }

    // Outputs
    JsonArrayConst outs = root["outputs"].as<JsonArrayConst>();
    if (outs.isNull() || outs.size() == 0) return fail("\"outputs\" must be a non-empty list");
    std::vector<int> usedBy(out.panels.size(), -1);
    std::map<int, std::string> pinOwner;
    auto claimPin = [&](JsonVariantConst v, const std::string &where, const char *what, uint8_t &dst) {
        if (!isInt(v) || v.as<long>() < 0 || v.as<long>() > 29) {
            error = where + ": " + what + " must be a GPIO number 0-29";
            return false;
        }
        int gpio = v.as<int>();
        if (isReservedPin(gpio)) {
            char msg[16];
            snprintf(msg, sizeof msg, "GP%d", gpio);
            error = where + ": " + what + " " + msg + " is used by ProtoFace itself (" + reservedPinList() + ")";
            return false;
        }
        if (pinOwner.count(gpio)) {
            char msg[16];
            snprintf(msg, sizeof msg, "GP%d", gpio);
            error = where + ": " + msg + " is already used by " + pinOwner[gpio];
            return false;
        }
        pinOwner[gpio] = where;
        dst = (uint8_t)gpio;
        return true;
    };

    int count[3] = {0, 0, 0};
    for (size_t oi = 0; oi < outs.size(); oi++) {
        JsonObjectConst o = outs[oi].as<JsonObjectConst>();
        Output output;
        const char *type = o.isNull() ? nullptr : o["type"].as<const char *>();
        if (!type) return fail("each output needs a \"type\"");
        if      (strcmp(type, "ws2812") == 0)  output.type = OUT_WS2812;
        else if (strcmp(type, "hub75") == 0)   output.type = OUT_HUB75;
        else if (strcmp(type, "max7219") == 0) output.type = OUT_MAX7219;
        else return fail(std::string("unknown output type ") + q(type) + " (use ws2812, hub75 or max7219)");
        count[output.type]++;
        char dflt[16];
        snprintf(dflt, sizeof dflt, "%u", (unsigned)oi);
        output.name = o["name"].is<const char *>() ? o["name"].as<const char *>() : dflt;
        std::string where = "output " + q(output.name);

        // Panels this output shows
        JsonArrayConst entries = o[output.type == OUT_HUB75 ? "place" : "chain"].as<JsonArrayConst>();
        if (entries.isNull() || entries.size() == 0)
            return fail(where + (output.type == OUT_HUB75 ? ": \"place\"" : ": \"chain\"") +
                        " must be a non-empty list");
        int outWiring, outStart;
        if (!parseEnum(o["wiring"], WIRING_NAMES, 4, 0, outWiring))
            return fail(where + ": wiring must be rows, serpentine, columns or columns_serpentine");
        if (!parseEnum(o["start"], START_NAMES, 4, 0, outStart))
            return fail(where + ": start must be top-left, top-right, bottom-left or bottom-right");
        for (JsonVariantConst e : entries) {
            Placement pl;
            std::string pid;
            JsonObjectConst eo = e.as<JsonObjectConst>();
            if (e.is<const char *>()) pid = e.as<const char *>();
            else if (!eo.isNull() && eo["panel"].is<const char *>()) pid = eo["panel"].as<const char *>();
            else return fail(where + ": each entry must be a panel id or an object with \"panel\"");
            int pi = out.panelIndex(pid);
            if (pi < 0) return fail(where + ": unknown panel " + q(pid));
            if (usedBy[pi] >= 0)
                return fail("panel " + q(pid) + " is used by output " + q(out.outputs.size() > (size_t)usedBy[pi]
                            ? out.outputs[usedBy[pi]].name : output.name) + " and " + where);
            usedBy[pi] = (int)oi;
            pl.panel = (uint16_t)pi;
            int f = 0, w = outWiring, s = outStart;
            if (!eo.isNull()) {
                if (!parseEnum(eo["flip"], FLIP_NAMES, 4, 0, f)) return fail(where + ": flip must be none, h, v or hv");
                if (!parseEnum(eo["wiring"], WIRING_NAMES, 4, outWiring, w)) return fail(where + ": bad wiring");
                if (!parseEnum(eo["start"], START_NAMES, 4, outStart, s)) return fail(where + ": bad start");
                if (!eo["rotate"].isNull()) {
                    long r = isInt(eo["rotate"]) ? eo["rotate"].as<long>() : -1;
                    if (r != 0 && r != 90 && r != 180 && r != 270)
                        return fail(where + ": rotate must be 0, 90, 180 or 270");
                    pl.rotate = (uint16_t)r;
                }
            }
            pl.flip = (uint8_t)f; pl.wiring = (uint8_t)w; pl.start = (uint8_t)s;
            const Panel &panel = out.panels[pi];
            int mw = (pl.rotate == 90 || pl.rotate == 270) ? panel.h : panel.w;
            int mh = (pl.rotate == 90 || pl.rotate == 270) ? panel.w : panel.h;
            if (output.type == OUT_MAX7219 && (panel.custom || mw != 8 || mh != 8))
                return fail(where + ": panel " + q(pid) + " must be an 8x8 grid for a MAX7219 module");
            if (output.type == OUT_HUB75) {
                if (eo.isNull() || !isInt(eo["x"]) || !isInt(eo["y"]) || eo["x"].as<long>() < 0 || eo["y"].as<long>() < 0)
                    return fail(where + ": " + q(pid) + " needs \"x\" and \"y\"");
                pl.x = eo["x"].as<uint16_t>();
                pl.y = eo["y"].as<uint16_t>();
            }
            output.place.push_back(pl);
        }

        if (output.type == OUT_WS2812) {
            if (!claimPin(o["pin"], where, "pin", output.pin)) return false;
            if (o["color_order"].is<const char *>()) output.colorOrder = o["color_order"].as<const char *>();
            static const char *const ORDERS[] = {"RGB", "RBG", "GRB", "GBR", "BRG", "BGR"};
            bool ok = false;
            for (const char *ord : ORDERS) ok |= output.colorOrder == ord;
            if (!ok) return fail(where + ": color_order must be RGB, RBG, GRB, GBR, BRG or BGR");
        } else if (output.type == OUT_HUB75) {
            if (!isInt(o["width"]) || !isInt(o["height"])) return fail(where + ": \"width\" and \"height\" are required");
            output.width  = o["width"].as<uint16_t>();
            output.height = o["height"].as<uint16_t>();
            output.chain  = isInt(o["chain"]) ? o["chain"].as<uint16_t>() : 1;
            output.scan   = isInt(o["scan"]) ? o["scan"].as<uint16_t>() : output.height / 2;
            if (output.width < 8 || output.width > 128 || output.height < 8 || output.height > 64 ||
                output.chain < 1 || output.chain > 4)
                return fail(where + ": panel size or chain out of range");
            JsonObjectConst pins = o["pins"].as<JsonObjectConst>();
            if (pins.isNull()) return fail(where + ": \"pins\" is required");
            JsonArrayConst rgb = pins["rgb"].as<JsonArrayConst>(), addr = pins["addr"].as<JsonArrayConst>();
            if (rgb.isNull() || rgb.size() != 6) return fail(where + ": pins.rgb must list 6 pins (R1 G1 B1 R2 G2 B2)");
            if (addr.isNull() || addr.size() < 3 || addr.size() > 5) return fail(where + ": pins.addr must list 3-5 pins (A B C D E)");
            for (JsonVariantConst v : rgb)  { uint8_t g; if (!claimPin(v, where, "rgb pin", g)) return false; output.rgbPins.push_back(g); }
            for (JsonVariantConst v : addr) { uint8_t g; if (!claimPin(v, where, "addr pin", g)) return false; output.addrPins.push_back(g); }
            if (!claimPin(pins["clk"], where, "clk pin", output.clkPin) ||
                !claimPin(pins["lat"], where, "lat pin", output.latPin) ||
                !claimPin(pins["oe"],  where, "oe pin",  output.oePin)) return false;
        } else {
            JsonObjectConst pins = o["pins"].as<JsonObjectConst>();
            if (pins.isNull()) return fail(where + ": \"pins\" is required");
            if (!claimPin(pins["din"], where, "din pin", output.dinPin) ||
                !claimPin(pins["clk"], where, "clk pin", output.spiClkPin) ||
                !claimPin(pins["cs"],  where, "cs pin",  output.csPin)) return false;
            if (isInt(o["threshold"])) output.threshold = (uint8_t)constrainByte(o["threshold"].as<long>());
            if (isInt(o["intensity"])) output.intensity = (uint8_t)(o["intensity"].as<long>() & 15);
        }

        // Physical index → frame pixel
        if (output.type == OUT_HUB75) {
            uint32_t fbw = (uint32_t)output.width * output.chain;
            output.length = fbw * output.height;
            output.source.assign(output.length, -1);
            for (const Placement &pl : output.place) {
                const Panel &p = out.panels[pl.panel];
                for (uint32_t i = 0; i < p.count(); i++) {
                    Point pt = p.at(i);
                    int x, y, mw, mh;
                    mount(p, pl, pt.x, pt.y, x, y, mw, mh);
                    uint32_t fx = pl.x + x, fy = pl.y + y;
                    if (fx >= fbw || fy >= output.height)
                        return fail(where + ": panel " + q(p.id) + " does not fit in the framebuffer");
                    output.source[fy * fbw + fx] = (int32_t)(p.offset + i);
                }
            }
        } else {
            uint32_t base = 0;
            for (const Placement &pl : output.place) base += out.panels[pl.panel].count();
            output.length = base;
            output.source.assign(output.length, -1);
            base = 0;
            for (const Placement &pl : output.place) {
                const Panel &p = out.panels[pl.panel];
                for (uint32_t i = 0; i < p.count(); i++)
                    output.source[base + chainIndex(p, pl, i)] = (int32_t)(p.offset + i);
                base += p.count();
            }
        }
        out.outputs.push_back(output);
    }

    for (size_t i = 0; i < out.panels.size(); i++)
        if (usedBy[i] < 0) return fail("panel " + q(out.panels[i].id) + " is not shown by any output");

    // Limits of this firmware version
    int kinds = (count[0] > 0) + (count[1] > 0) + (count[2] > 0);
    if (kinds > 1) return fail("this firmware drives one output type at a time");
    if (count[OUT_WS2812] > MAX_WS2812) return fail("this firmware drives at most 4 ws2812 outputs");
    if (count[OUT_HUB75] > 1 || count[OUT_MAX7219] > 1)
        return fail("this firmware drives one hub75 or max7219 output");
    return true;
}

// ── Built-in "NO SD" face ─────────────────────────────────────────────────────
namespace {

const uint8_t TEXT_RGB[3] = {255, 0, 0};
const uint8_t WARN_RGB[3] = {255, 170, 0};

// "NO SD" in 4x7 letters (boxes under 30 px wide) or 5x7 letters
const char *const GLYPHS4[4][7] = {
    {"1001", "1101", "1101", "1011", "1011", "1001", "1001"},   // N
    {"0110", "1001", "1001", "1001", "1001", "1001", "0110"},   // O
    {"0111", "1000", "1000", "0110", "0001", "0001", "1110"},   // S
    {"1110", "1001", "1001", "1001", "1001", "1001", "1110"},   // D
};
const char *const GLYPHS5[4][7] = {
    {"10001", "10001", "11001", "10101", "10011", "10001", "10001"},
    {"01110", "10001", "10001", "10001", "10001", "10001", "01110"},
    {"01111", "10000", "10000", "01110", "00001", "00001", "11110"},
    {"11110", "10001", "10001", "10001", "10001", "10001", "11110"},
};
// width of "NO SD": two 2-letter words, 1-px letter gap, word gap 3 (4x7) or 4 (5x7)
int textWidth(bool big) { return big ? 5 * 4 + 2 + 4 : 4 * 4 + 2 + 3; }

struct Box { int x0, y0, x1, y1; };     // canvas coords, x1/y1 exclusive

struct Painter {
    const DisplayConfig &cfg;
    const std::vector<int> &panels;     // panels we may draw on
    uint8_t *rgb;
    void put(int cx, int cy, const uint8_t c[3]) {
        for (int pi : panels) {
            const Panel &p = cfg.panels[pi];
            int32_t j = p.indexOf(cx - p.x, cy - p.y);
            if (j < 0) continue;
            memcpy(rgb + (p.offset + j) * 3, c, 3);
        }
    }
    void text(const Box &b) {
        bool big = b.x1 - b.x0 >= 30;
        int cw = big ? 5 : 4, gap = big ? 4 : 3;
        int x0 = b.x0 + (b.x1 - b.x0 - textWidth(big)) / 2;
        int y0 = b.y0 + (b.y1 - b.y0 - 6) / 2;
        for (int ch = 0; ch < 4; ch++) {
            int ox = x0 + (ch % 2) * (cw + 1) + (ch / 2) * (2 * cw + 1 + gap);
            for (int y = 0; y < 7; y++)
                for (int x = 0; x < cw; x++)
                    if ((big ? GLYPHS5 : GLYPHS4)[ch][y][x] == '1') put(ox + x, y0 + y, TEXT_RGB);
        }
    }
    // Warning triangle with a dark "!", as big as fits in the box
    void triangle(const Box &b) {
        int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
        int h = bh < (bw + 1) / 2 ? bh : (bw + 1) / 2;
        if (h < 3) { put(b.x0 + bw / 2, b.y0 + bh / 2, WARN_RGB); return; }
        int cx = b.x0 + bw / 2, top = b.y0 + (bh - h) / 2;
        for (int y = 0; y < h; y++)
            for (int x = cx - y - 1; x <= cx + y; x++) {
                bool bang = (x == cx - 1 || x == cx) &&
                            ((y >= h * 3 / 10 && y <= h * 6 / 10) || y == h - 2);
                if (!bang) put(x, top + y, WARN_RGB);
            }
    }
    // Narrower, odd-width triangle for a nose-sized box
    void smallTriangle(const Box &b) {
        int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
        int h = bh - 1;
        if (h > bw) h = bw;
        if (h < 3) { triangle(b); return; }
        int cx = b.x0 + (bw - 1) / 2, top = b.y1 - h;
        for (int r = 0; r < h; r++)
            for (int x = cx - r / 2; x <= cx + r / 2; x++) {
                bool bang = x == cx && ((r >= h * 2 / 7 && r <= h * 3 / 7) || r == h - 2);
                if (!bang) put(x, top + r, WARN_RGB);
            }
    }
};

}  // namespace

void drawNoSdFace(const DisplayConfig &cfg, uint8_t *rgb) {
    memset(rgb, 0, cfg.totalPixels * 3);
    std::vector<int> drawable;
    for (size_t i = 0; i < cfg.panels.size(); i++)
        if (!cfg.mirror.enabled || cfg.panels[i].side == cfg.mirror.from) drawable.push_back((int)i);

    // Group the drawable panels by (side, region); plain panels stand alone.
    std::vector<std::pair<std::string, std::vector<int>>> groups;
    for (int pi : drawable) {
        const Panel &p = cfg.panels[pi];
        std::string key = p.region.empty() ? "#" + p.id : p.side + "/" + p.region;
        size_t k = 0;
        while (k < groups.size() && groups[k].first != key) k++;
        if (k == groups.size()) groups.push_back({key, {}});
        groups[k].second.push_back(pi);
    }
    for (auto &g : groups) {
        Box b{1 << 30, 1 << 30, -1, -1};
        for (int pi : g.second) {
            const Panel &p = cfg.panels[pi];
            if (p.x < b.x0) b.x0 = p.x;
            if (p.y < b.y0) b.y0 = p.y;
            if (p.x + p.w > b.x1) b.x1 = p.x + p.w;
            if (p.y + p.h > b.y1) b.y1 = p.y + p.h;
        }
        Painter paint{cfg, g.second, rgb};
        const Panel &first = cfg.panels[g.second[0]];
        bool fitsText = b.x1 - b.x0 >= textWidth(false) && b.y1 - b.y0 >= 7;
        if (first.region == "nose") {
            paint.smallTriangle(b);
        } else if (first.region == "eye" || !fitsText) {
            paint.triangle(b);
        } else if (first.region.empty() && b.y1 - b.y0 >= 16) {
            // A big plain panel: triangle above the text
            Box top{b.x0, b.y0, b.x1, b.y0 + (b.y1 - b.y0) / 2};
            Box bottom{b.x0, top.y1, b.x1, b.y1};
            paint.triangle(top);
            paint.text(bottom);
        } else {
            paint.text(b);
        }
    }
    if (cfg.mirror.enabled) {
        std::vector<int32_t> m = cfg.mirrorMap();
        for (const Panel &p : cfg.panels) {
            if (p.side != cfg.mirror.from) continue;
            for (uint32_t i = 0; i < p.count(); i++) {
                int32_t j = m[p.offset + i];
                if (j >= 0) memcpy(rgb + j * 3, rgb + (p.offset + i) * 3, 3);
            }
        }
    }
}

}  // namespace pf
