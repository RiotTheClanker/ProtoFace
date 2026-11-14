// Host-side tests for src/pf_config.*.
// Build and run:  make -C tests/host   (needs ArduinoJson 7 in tests/host/ArduinoJson/)
#include "../../src/pf_config.h"

#include <stdio.h>
#include <string.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string readFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static const std::string ROOT = "../../";

static pf::DisplayConfig load(const std::string &rel) {
    std::string json = readFile(ROOT + rel), err;
    pf::DisplayConfig cfg;
    bool ok = pf::parseConfig(json.data(), json.size(), cfg, err);
    if (!ok) printf("  %s: %s\n", rel.c_str(), err.c_str());
    CHECK(ok);
    return cfg;
}

static std::string rejectReason(const std::string &json) {
    pf::DisplayConfig cfg;
    std::string err;
    bool ok = pf::parseConfig(json.data(), json.size(), cfg, err);
    return ok ? "" : err;
}

// V6.2 placed logical panel l (in panel order on its side) at chain position map[l],
// with rows wiring and no flips.  Check the config reproduces that exactly.
static void checkV62Map(const pf::DisplayConfig &cfg, int outIdx, const std::vector<int> &map,
                        uint32_t sideOffset) {
    const pf::Output &o = cfg.outputs[outIdx];
    CHECK(o.length == map.size() * 64);
    for (size_t l = 0; l < map.size(); l++)
        for (int j = 0; j < 64; j++)
            CHECK(o.source[map[l] * 64 + j] == (int32_t)(sideOffset + l * 64 + j));
}

// Parse FALLBACK_LEDS out of a V6.2 fallback header.
static std::vector<uint8_t> fallbackRgb(const std::string &path) {
    std::string h = readFile(path);
    size_t p = h.find("FALLBACK_LEDS");
    p = h.find('{', p) + 1;
    std::vector<uint8_t> rgb;
    int r, g, b, sm, pr;
    while ((p = h.find('{', p)) != std::string::npos) {
        if (sscanf(h.c_str() + p, "{%d,%d,%d,%d,%d}", &r, &g, &b, &sm, &pr) != 5) break;
        rgb.push_back(r); rgb.push_back(g); rgb.push_back(b);
        p++;
    }
    return rgb;
}

int main() {
    // ── Presets reproduce V6.2 ────────────────────────────────────────────────
    pf::DisplayConfig mk2 = load("displays/mk2.json");
    CHECK(mk2.totalPixels == 704);
    CHECK(mk2.layoutId() == 0x2F5051C2u);           // same as display_config.py
    CHECK(mk2.v1Compatible(11) && !mk2.v1Compatible(14));
    CHECK(mk2.outputs.size() == 2 && mk2.outputs[0].pin == 2 && mk2.outputs[1].pin == 3);
    checkV62Map(mk2, 0, {0, 1, 2, 3, 4, 5}, 0);
    checkV62Map(mk2, 1, {3, 4, 0, 1, 2}, 384);

    pf::DisplayConfig mk3 = load("displays/mk3.json");
    CHECK(mk3.totalPixels == 896);
    CHECK(mk3.layoutId() == 0x6FAFE750u);
    checkV62Map(mk3, 0, {6, 5, 4, 3, 2, 1, 0}, 0);
    checkV62Map(mk3, 1, {0, 1, 2, 3, 4, 5, 6}, 448);

    // Mirror map: left eye1 (0,0) -> right eye2 (7,0), as in the tool
    std::vector<int32_t> m = mk3.mirrorMap();
    CHECK(m[mk3.panels[mk3.panelIndex("L_eye1")].offset] ==
          (int32_t)mk3.panels[mk3.panelIndex("R_eye2")].offset + 7);

    // ── Examples (layout IDs match display_config.py) ─────────────────────────
    pf::DisplayConfig hub2 = load("displays/examples/hub75-2x64x32.json");
    CHECK(hub2.layoutId() == 0x96B51148u);
    CHECK(hub2.outputs[0].type == pf::OUT_HUB75 && hub2.outputs[0].length == 128 * 32);
    CHECK(hub2.outputs[0].source[64] == 2048);       // Right panel starts at x = 64
    CHECK(hub2.mirrorMap()[0] == 2048 + 63);
    pf::DisplayConfig hub1 = load("displays/examples/hub75-1x64x32.json");
    CHECK(hub1.layoutId() == 0x10738CC3u);
    pf::DisplayConfig mx = load("displays/examples/max7219-single-side.json");
    CHECK(mx.layoutId() == 0xB9EF230Fu);
    // nose (last module) is rotated 180: its first frame pixel is the module's last LED
    CHECK(mx.outputs[0].source[6 * 64 + 63] == (int32_t)mx.panels[6].offset);
    pf::DisplayConfig eye = load("displays/examples/neopixel-custom-eye.json");
    CHECK(eye.layoutId() == 0x728ED9CFu);
    CHECK(eye.totalPixels == 64 && eye.outputs[0].source[33] == 33);

    // ── Wiring transforms ─────────────────────────────────────────────────────
    pf::Panel p; p.w = 4; p.h = 3;
    pf::Placement pl;
    pl.wiring = pf::WIRE_SERPENTINE;
    CHECK(pf::chainIndex(p, pl, 4) == 7);            // (0,1) is the end of reversed row 1
    pl.wiring = pf::WIRE_ROWS; pl.start = pf::START_TOP_RIGHT;
    CHECK(pf::chainIndex(p, pl, 0) == 3);
    pl.start = pf::START_TOP_LEFT; pl.rotate = 90;  // 3 wide x 4 tall once mounted
    CHECK(pf::chainIndex(p, pl, 0) == 2);            // top-left goes to top-right
    pl.rotate = 0; pl.flip = pf::FLIP_V;
    CHECK(pf::chainIndex(p, pl, 0) == 8);
    pl.flip = pf::FLIP_NONE; pl.wiring = pf::WIRE_COLUMNS;
    CHECK(pf::chainIndex(p, pl, 1) == 3);

    // ── Rejections ────────────────────────────────────────────────────────────
    std::string base = readFile(ROOT + "displays/mk2.json");
    auto with = [&](const std::string &from, const std::string &to) {
        std::string s = base;
        size_t at = s.find(from);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) s.replace(at, from.size(), to);
        return s;
    };
    CHECK(rejectReason(with("\"pin\": 2", "\"pin\": 17")).find("used by ProtoFace itself") != std::string::npos);
    CHECK(rejectReason(with("\"pin\": 2", "\"pin\": 26")).find("mic") != std::string::npos);
    CHECK(rejectReason(with("\"pin\": 2", "\"pin\": 3")).find("already used") != std::string::npos);
    CHECK(rejectReason(with("\"L_eye1\", \"L_eye2\"", "\"L_eye1\"")).find("not shown") != std::string::npos);
    CHECK(rejectReason(with("\"L_eye1\", \"L_eye2\"", "\"L_eye1\", \"L_eye1\"")).find("is used by") != std::string::npos);
    CHECK(rejectReason(with("\"protoface_display\": 1", "\"protoface_display\": 2")) != "");
    CHECK(rejectReason("{not json").find("not valid JSON") == 0);
    CHECK(rejectReason(with("\"type\": \"ws2812\",\n      \"name\": \"right\"",
                            "\"type\": \"max7219\",\n      \"name\": \"right\"")) != "");

    CHECK(pf::crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);

    // ── Built-in "NO SD" face matches the V6.2 Mk2 frame ──────────────────────
    {
        std::vector<uint8_t> rgb(mk2.totalPixels * 3);
        pf::drawNoSdFace(mk2, rgb.data());
        std::vector<uint8_t> want = fallbackRgb(ROOT + "tests/host/v62/mk2_fallback.h");
        CHECK(want.size() == rgb.size());
        CHECK(want == rgb);

        std::vector<uint8_t> rgb3(mk3.totalPixels * 3);
        pf::drawNoSdFace(mk3, rgb3.data());
        std::vector<uint8_t> want3 = fallbackRgb(ROOT + "tests/host/v62/mk3_fallback.h");
        // Same except the right nose, which is now mirrored like the rest of that side
        uint32_t rn = mk3.panels[mk3.panelIndex("R_nose")].offset * 3;
        CHECK(want3.size() == rgb3.size());
        bool same = true;
        for (size_t i = 0; i < rgb3.size() && i < want3.size(); i++)
            if ((i < rn || i >= rn + 64 * 3) && rgb3[i] != want3[i]) same = false;
        CHECK(same);

        std::vector<uint8_t> rgbh(hub2.totalPixels * 3);
        pf::drawNoSdFace(hub2, rgbh.data());
        int lit = 0;
        for (size_t i = 0; i < rgbh.size(); i += 3) lit += rgbh[i] || rgbh[i + 1];
        CHECK(lit > 100);
    }

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
