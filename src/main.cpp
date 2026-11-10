#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>
#include <BTstackLib.h>
#include <BluetoothLock.h>
#include <Wire.h>
#include <Adafruit_HUSB238.h>
#include <vector>

#include "pf_config.h"

// extern "C" must be at file scope, not inline with #include
extern "C" {
#include "ble/att_server.h"
}

#define FIRMWARE_VERSION "V7.0-dev"

// ── Display ───────────────────────────────────────────────────────────────────
//
//  The panels, their LED pins and wiring come from a display config
//  (display.json, see docs/display-config.md), not from build flags.  Put it
//  in the root of the SD card: at boot (and on 'reload') a new or changed
//  display.json is checked and copied into the Pico's flash, so the helmet
//  keeps working without the SD card.  A broken file on the card never
//  replaces a working copy in flash.  With no config at all nothing lights up.
//
//  The SD card, microphone and USB-PD pins below are fixed; a display.json
//  that tries to use them is refused (see pf::isReservedPin).
//
#define DISPLAY_FILE "/display.json"

// ── Debug ─────────────────────────────────────────────────────────────────────
#define DEBUG_BLE 0

// ── Pins ──────────────────────────────────────────────────────────────────────
#define SD_CS_PIN      17
#define SD_MISO_PIN    16
#define SD_SCK_PIN     18
#define SD_MOSI_PIN    19
#define SOUND_AO_PIN   26   // SPW2430 DC (analog) output → ADC0

// ── USB-C Power Delivery (HUSB238 over I2C) ───────────────────────────────────
#define PD_I2C_SDA          4       // HUSB238 SDA  (Pico I2C0) — change to match wiring
#define PD_I2C_SCL          5       // HUSB238 SCL  (Pico I2C0)
#define PD_HEADROOM         0.85f   // only budget this fraction of advertised current
#define BASE_SYSTEM_MA      300.0f  // reserve for Pico 2W + mic + SD + logic
#define DEFAULT_USB_MA      500.0f  // assumed budget when no HUSB238 is present

// ── LED power model (SK6805-EC15) ─────────────────────────────────────────────
#define LED_FULL_WHITE_MA   15.0f   // current one LED draws at full white (all 3 ch)
#define LED_QUIESCENT_MA    1.0f    // per-LED controller draw at ~zero output (not dimmable)
#define LED_MIN_BRIGHTNESS  40      // power limiting never dims below this (0-255)

// ── Sound / timing modes ──────────────────────────────────────────────────────
#define SOUND_STATIC  0
#define SOUND_SNAP    1
#define SOUND_LINEAR  2
#define TIMING_TIMED  0
#define TIMING_SOUND  1

// SOUND-timed frames advance when the volume rises through SOUND_TRIGGER_LEVEL
// (after being shown for at least duration_ms).  The volume must fall back
// below SOUND_TRIGGER_RELEASE before the next frame can trigger.
#define SOUND_TRIGGER_LEVEL    128
#define SOUND_TRIGGER_RELEASE  96


// ── .anim file layout ─────────────────────────────────────────────────────────
#define ANIM_V1_HEADER  8
#define ANIM_V2_HEADER  16
#define FRAME_HDR_SIZE  4

struct LEDEntry {
    uint8_t r, g, b;
    uint8_t sound_mode;
    uint8_t param;
};
// The .anim reader copies raw 5-byte records straight into LEDEntry.
static_assert(sizeof(LEDEntry) == 5, "LEDEntry must be exactly 5 bytes");

struct AnimFrame {
    uint16_t duration_ms = 1000;
    uint8_t  timing_mode = 0;
    std::vector<LEDEntry> leds;
};

// ── NUS UUIDs ─────────────────────────────────────────────────────────────────
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"


// ── Display state ─────────────────────────────────────────────────────────────
pf::DisplayConfig display;
bool              haveDisplay = false;     // a usable config is loaded
String            displayProblem;          // why not, for 'status' / 'display'
std::vector<Adafruit_NeoPixel *> strips;   // one per ws2812 output

// ── Playback state ────────────────────────────────────────────────────────────
AnimFrame     currentFrame;
bool          playing        = true;
bool          sdReady        = false;
unsigned long lastFrameTime  = 0;
bool          triggerArmed   = true;
uint8_t       brightness     = 255;   // user brightness ('bright' command)
bool          identMode      = false; // show chain positions instead of the face

// ── Power state ───────────────────────────────────────────────────────────────
Adafruit_HUSB238 husb;
bool    pdReady         = false;   // HUSB238 present & responding
uint8_t pdVolts         = 5;       // negotiated source voltage
float   availableAmps   = 0.0f;    // current available at pdVolts
float   ledBudgetMA     = 0.0f;    // current the LEDs may draw (after reserve/headroom)
uint8_t lastPowerScale  = 255;     // power-limit dimming applied to the last frame (0-255)

#define MAX_FILES 32
char fileList[MAX_FILES][64];
int  fileCount       = 0;
int  fileIndex       = 0;
int  currentFrameIdx = 0;
int  totalFrames     = 0;
long animHeaderSize  = ANIM_V1_HEADER;
long frameSize       = 0;
File animFile;

AnimFrame fallbackFrame;              // built-in "NO SD" face, drawn from the config
bool      useFallback = true;

unsigned long rebootAt = 0;           // restart after a new config is saved

// ── BLE state ─────────────────────────────────────────────────────────────────
// BTstack callbacks run outside loop(), so they only hand data over through
// these buffers; commands are executed and replies sent from loop().
static volatile hci_con_handle_t conn_handle     = HCI_CON_HANDLE_INVALID;
static uint16_t                  tx_value_handle = 0;
static uint16_t                  rx_value_handle = 0;
static volatile bool             ble_subscribed  = false;
static volatile bool             ble_greet       = false;

#define CMD_MAX 256
static char          pendingCmd[CMD_MAX];
static volatile bool cmdPending = false;

#define TX_BUF_SIZE 4096
static char     txBuf[TX_BUF_SIZE];
static uint16_t txHead = 0, txTail = 0;   // ring buffer of outgoing text

// ── BLE helpers ───────────────────────────────────────────────────────────────
void bleSend(const char *msg) {
    if (!ble_subscribed || conn_handle == HCI_CON_HANDLE_INVALID) return;
    for (const char *p = msg; *p; p++) {
        uint16_t next = (txHead + 1) % TX_BUF_SIZE;
        if (next == txTail) break;          // buffer full — drop the rest
        txBuf[txHead] = *p;
        txHead = next;
    }
}

void bleSendLine(const char *msg) {
    bleSend(msg);
    bleSend("\r\n");
}

void bleSendLine(const String &msg) { bleSendLine(msg.c_str()); }

// Send queued text as notifications, split to fit the negotiated MTU.
void bleFlush() {
    if (txHead == txTail) return;
    hci_con_handle_t con = conn_handle;
    if (!ble_subscribed || con == HCI_CON_HANDLE_INVALID) {
        txHead = txTail = 0;
        return;
    }
    uint8_t chunk[244];
    for (int budget = 8; budget > 0 && txHead != txTail; budget--) {
        BluetoothLock lock;
        if (!att_server_can_send_packet_now(con)) break;
        uint16_t maxLen = att_server_get_mtu(con) - 3;
        if (maxLen > sizeof(chunk)) maxLen = sizeof(chunk);
        uint16_t n = 0, t = txTail;
        while (n < maxLen && t != txHead) {
            chunk[n++] = txBuf[t];
            t = (t + 1) % TX_BUF_SIZE;
        }
        if (att_server_notify(con, tx_value_handle, chunk, n) != ERROR_CODE_SUCCESS)
            break;
        txTail = t;
    }
}


// ── ADC / Microphone ──────────────────────────────────────────────────────────
// The SPW2430's DC (analog) output rides on a DC bias voltage; audio is a small
// AC swing on top of it. Reading the raw pin mostly returns that fixed bias, so
// quiet and loud sound almost the same. Instead we sample a short window, take
// the peak-to-peak swing (which cancels the DC bias), subtract an ambient noise
// floor, scale to 0-255, and smooth with a fast-attack / slow-decay envelope so
// brightness tracks perceived loudness. All tunable below.
#define MIC_SAMPLES      256    // samples taken per volume measurement
#define MIC_NOISE_FLOOR  40     // 12-bit counts of ambient/self noise to ignore
#define MIC_FULLSCALE    900    // peak-to-peak counts that map to full (255)
#define MIC_ATTACK       0.60f  // 0..1 — how fast the envelope rises on louder sound
#define MIC_DECAY        0.08f  // 0..1 — how fast the envelope falls when quieter

uint8_t readVolume() {
    uint16_t vmin = 4095, vmax = 0;
    for (int i = 0; i < MIC_SAMPLES; i++) {
        uint16_t v = analogRead(SOUND_AO_PIN);
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    int pp = (int)vmax - (int)vmin - MIC_NOISE_FLOOR;   // audio swing above the floor
    if (pp < 0) pp = 0;

    float norm = (float)pp / (float)(MIC_FULLSCALE - MIC_NOISE_FLOOR);
    if (norm > 1.0f) norm = 1.0f;
    norm = sqrtf(norm);                                 // perceptual (loudness) curve
    float target = norm * 255.0f;

    // Fast-attack / slow-decay envelope so the LEDs punch on beats but don't flicker.
    static float env = 0.0f;
    float rate = (target > env) ? MIC_ATTACK : MIC_DECAY;
    env += (target - env) * rate;

    return (uint8_t)(env + 0.5f);
}


// ── USB-C Power Delivery / LED current budget ─────────────────────────────────
// Map the HUSB238 current enums to amps. Enumerators are ordered 0..15 / 0..3.
static float currentSettingToAmps(HUSB238_CurrentSetting c) {
    static const float A[16] = {0.5f,0.7f,1.0f,1.25f,1.5f,1.75f,2.0f,2.25f,
                                2.5f,2.75f,3.0f,3.25f,3.5f,4.0f,4.5f,5.0f};
    int i = (int)c;
    return (i >= 0 && i < 16) ? A[i] : 0.0f;
}
static float contract5VToAmps(HUSB238_5VCurrentContract c) {
    switch (c) {
        case CURRENT5V_1_5_A: return 1.5f;
        case CURRENT5V_2_4_A: return 2.4f;
        case CURRENT5V_3_A:   return 3.0f;
        default:              return 0.9f;   // CURRENT5V_DEFAULT (plain USB-C Rp)
    }
}

// Convert the available current into an LED current budget (mA). The actual
// dimming is done per-frame in pushFrame() from each frame's estimated draw,
// so frames that don't peg every LED to white stay at full brightness.
void applyPowerBudget() {
    float usableMA = availableAmps * 1000.0f * PD_HEADROOM - BASE_SYSTEM_MA;
    if (usableMA < 0) usableMA = 0;
    ledBudgetMA = usableMA;
}

// Negotiate 5V over USB-C PD (I2C) and set the LED current budget accordingly.
void initPower() {
    Wire.setSDA(PD_I2C_SDA);
    Wire.setSCL(PD_I2C_SCL);
    Wire.begin();

    if (!husb.begin(HUSB238_I2CADDR_DEFAULT, &Wire)) {
        Serial.println("HUSB238 not found — assuming plain USB (5V, 0.5A)");
        pdReady       = false;
        pdVolts       = 5;
        availableAmps = DEFAULT_USB_MA / 1000.0f;
        applyPowerBudget();
        return;
    }
    pdReady = true;

    // Ask the attached charger what it can supply, then read the 5V capability.
    husb.getSourceCapabilities();
    float amps5v;
    if (husb.isVoltageDetected(PD_SRC_5V)) {
        amps5v = currentSettingToAmps(husb.currentDetected(PD_SRC_5V));
    } else {
        amps5v = contract5VToAmps(husb.get5VContractA());
    }

    // We run the LEDs at 5V; request it and take what we get.
    husb.selectPD(PD_SRC_5V);
    husb.requestPD();
    delay(50);
    if (husb.getPDSrcVoltage() == PD_5V) {
        float negotiated = currentSettingToAmps(husb.getPDSrcCurrent());
        if (negotiated > amps5v) amps5v = negotiated;
    }

    pdVolts       = 5;
    availableAmps = amps5v;
    applyPowerBudget();

    Serial.print("PD: 5V @ ");
    Serial.print(availableAmps, 2);
    Serial.print("A available  → LED budget ");
    Serial.print((int)ledBudgetMA);
    Serial.println(" mA (dynamic per-frame dimming)");
}


// ── Sound reaction ────────────────────────────────────────────────────────────
void applySound(const LEDEntry &led, uint8_t vol,
                uint8_t &r, uint8_t &g, uint8_t &b) {
    switch (led.sound_mode) {
        case SOUND_SNAP: {
            bool on = (vol >= led.param);
            r = on ? led.r : 0;
            g = on ? led.g : 0;
            b = on ? led.b : 0;
            break;
        }
        case SOUND_LINEAR: {
            float m  = (led.param >> 4) & 0x0F;
            float bv =  led.param & 0x0F;
            float sc = constrain(m * vol / 255.0f + bv / 15.0f, 0.0f, 1.0f);
            r = (uint8_t)(led.r * sc);
            g = (uint8_t)(led.g * sc);
            b = (uint8_t)(led.b * sc);
            break;
        }
        case SOUND_STATIC:
        default:
            r = led.r; g = led.g; b = led.b; break;
    }
}


// ── Display setup ─────────────────────────────────────────────────────────────
static neoPixelType neoOrder(const std::string &order) {
    if (order == "RGB") return NEO_RGB;
    if (order == "RBG") return NEO_RBG;
    if (order == "GBR") return NEO_GBR;
    if (order == "BRG") return NEO_BRG;
    if (order == "BGR") return NEO_BGR;
    return NEO_GRB;
}

static const char *outputTypeName(pf::OutputType t) {
    return t == pf::OUT_HUB75 ? "hub75" : t == pf::OUT_MAX7219 ? "max7219" : "ws2812";
}

static String readFlashDisplay() {
    File f = LittleFS.open(DISPLAY_FILE, "r");
    if (!f) return String();
    String s = f.readString();
    f.close();
    return s;
}

#define MAX_DISPLAY_JSON (48 * 1024)

// Copy a new or changed display.json from the SD card into flash.
// Returns true if flash now holds a different config than before.
bool syncDisplayFromSD() {
    if (!sdReady || !SD.exists(DISPLAY_FILE)) return false;
    File f = SD.open(DISPLAY_FILE);
    if (!f) return false;
    if (f.size() > MAX_DISPLAY_JSON) {
        f.close();
        displayProblem = "display.json on the SD card is too big (48 KB max)";
        Serial.println(displayProblem);
        bleSendLine(displayProblem);
        return false;
    }
    String json = f.readString();
    f.close();
    if (json == readFlashDisplay()) return false;          // already in flash

    pf::DisplayConfig check;
    std::string err;
    if (!pf::parseConfig(json.c_str(), json.length(), check, err)) {
        String msg = String("display.json on the SD card was not used: ") + err.c_str();
        Serial.println(msg);
        bleSendLine(msg);
        displayProblem = msg;
        return false;
    }
    File out = LittleFS.open(DISPLAY_FILE, "w");
    bool ok = out && out.write((const uint8_t *)json.c_str(), json.length()) == json.length();
    if (out) out.close();
    String msg = ok ? String("New display.json from the SD card saved: \"") + check.name.c_str() + "\""
                    : String("display.json could not be saved to flash");
    Serial.println(msg);
    bleSendLine(msg);
    return ok;
}

// Load /display.json from flash and start its outputs.
void initDisplay() {
    haveDisplay = false;
    String json = readFlashDisplay();
    if (json.length() == 0) {
        if (displayProblem.length() == 0)
            displayProblem = "no display set up: put a display.json on the SD card and restart";
        return;
    }
    std::string err;
    if (!pf::parseConfig(json.c_str(), json.length(), display, err)) {
        displayProblem = String("display.json in flash is not usable: ") + err.c_str();
        return;
    }
    for (const pf::Output &o : display.outputs) {
        if (o.type != pf::OUT_WS2812) {
            displayProblem = String(outputTypeName(o.type)) +
                             " outputs are not supported by this firmware yet";
            return;
        }
    }
    for (const pf::Output &o : display.outputs) {
        Adafruit_NeoPixel *s = new Adafruit_NeoPixel(o.length, o.pin, neoOrder(o.colorOrder) + NEO_KHZ800);
        s->begin();
        s->clear();
        s->show();
        strips.push_back(s);
    }
    currentFrame.leds.assign(display.totalPixels, LEDEntry{0, 0, 0, 0, 0});
    frameSize = FRAME_HDR_SIZE + (long)display.totalPixels * (long)sizeof(LEDEntry);

    std::vector<uint8_t> rgb(display.totalPixels * 3);
    pf::drawNoSdFace(display, rgb.data());
    fallbackFrame.leds.resize(display.totalPixels);
    for (uint32_t i = 0; i < display.totalPixels; i++)
        fallbackFrame.leds[i] = LEDEntry{rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2], 0, 0};
    fallbackFrame.duration_ms = 1000;
    fallbackFrame.timing_mode = TIMING_TIMED;
    haveDisplay = true;
    displayProblem = "";
}

// ── Ident pattern ─────────────────────────────────────────────────────────────
// Each panel shows its position on its output's chain as a digit, drawn
// straight into the panel's LED order (rows from its first LED; no flips or
// rotation applied): one colour per output, red dot on the panel's first LED.
// A mirrored or upside-down digit means that panel needs a flip in the config.
static const uint8_t DIGITS_3x5[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,2,2,2}, {7,5,7,5,7}, {7,5,7,1,7},
};
static const uint8_t IDENT_RGB[4][3] = {{0, 60, 0}, {0, 0, 60}, {50, 0, 50}, {0, 50, 50}};

void pushIdent() {
    for (size_t oi = 0; oi < strips.size(); oi++) {
        Adafruit_NeoPixel &strip = *strips[oi];
        const pf::Output &o = display.outputs[oi];
        const uint8_t *c = IDENT_RGB[oi % 4];
        strip.clear();
        uint32_t base = 0;
        for (size_t k = 0; k < o.place.size(); k++) {
            const pf::Placement &pl = o.place[k];
            const pf::Panel &p = display.panels[pl.panel];
            int w = (pl.rotate == 90 || pl.rotate == 270) ? p.h : p.w;
            int h = (pl.rotate == 90 || pl.rotate == 270) ? p.w : p.h;
            if (!p.custom && w >= 3 && h >= 5) {
                const uint8_t *d = DIGITS_3x5[k % 10];
                int x0 = (w - 3) / 2, y0 = (h - 5) / 2;
                for (int y = 0; y < 5; y++)
                    for (int x = 0; x < 3; x++)
                        if (d[y] & (4 >> x))
                            strip.setPixelColor(base + (y0 + y) * w + (x0 + x), c[0], c[1], c[2]);
            } else {
                for (uint32_t i = 1; i < p.count(); i++) strip.setPixelColor(base + i, c[0] / 4, c[1] / 4, c[2] / 4);
            }
            strip.setPixelColor(base, 60, 0, 0);
            base += p.count();
        }
        strip.show();
    }
}

// ── Push frame ────────────────────────────────────────────────────────────────
// Scratch buffers hold one frame's final colours (after sound reaction) in
// frame order, so the frame's current draw can be measured before writing.
static std::vector<uint8_t> frameRGB;

void pushFrame(const LEDEntry *leds, uint8_t vol) {
    const uint32_t n = display.totalPixels;
    if (frameRGB.size() != n * 3) frameRGB.assign(n * 3, 0);

    // ── Pass 1: resolve final colours (sound + user brightness), sum levels ───
    uint32_t sumCh = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t r, g, b;
        applySound(leds[i], vol, r, g, b);
        r = (uint16_t)r * brightness / 255;
        g = (uint16_t)g * brightness / 255;
        b = (uint16_t)b * brightness / 255;
        frameRGB[i * 3] = r; frameRGB[i * 3 + 1] = g; frameRGB[i * 3 + 2] = b;
        sumCh += (uint32_t)r + g + b;
    }

    // ── Estimate this frame's current draw ────────────────────────────────────
    //   dynamic   = (sum of channel levels / 255) × per-channel full current
    //   quiescent = fixed per-LED controller draw (cannot be dimmed away)
    float dynMA       = (float)sumCh / 255.0f * (display.limits.maPerLedWhite / 3.0f);
    float quiescentMA = (float)n * display.limits.maPerLedIdle;

    // ── Only dim if this frame would exceed the power budget ──────────────────
    uint16_t scale = 255;
    if (ledBudgetMA > 0.0f && (dynMA + quiescentMA) > ledBudgetMA && dynMA > 0.0f) {
        float avail = ledBudgetMA - quiescentMA;   // current left for the colour part
        if (avail < 0) avail = 0;
        int s = (int)(avail / dynMA * 255.0f);
        if (s < display.limits.minBrightness) s = display.limits.minBrightness;
        if (s > 255) s = 255;
        scale = (uint16_t)s;
    }
    lastPowerScale = (uint8_t)scale;

    // ── Pass 2: write each output in its physical order, scale applied ────────
    for (size_t oi = 0; oi < strips.size(); oi++) {
        Adafruit_NeoPixel &strip = *strips[oi];
        const std::vector<int32_t> &src = display.outputs[oi].source;
        for (uint32_t k = 0; k < src.size(); k++) {
            int32_t i = src[k];
            if (i < 0) { strip.setPixelColor(k, 0); continue; }
            strip.setPixelColor(k, (uint16_t)frameRGB[i * 3]     * scale / 255,
                                   (uint16_t)frameRGB[i * 3 + 1] * scale / 255,
                                   (uint16_t)frameRGB[i * 3 + 2] * scale / 255);
        }
        strip.show();
    }
}

void blankOutputs() {
    for (Adafruit_NeoPixel *s : strips) { s->clear(); s->show(); }
}

// ── SD helpers ────────────────────────────────────────────────────────────────
static bool isAnimName(const char *name) {
    size_t n = strlen(name);
    return n > 5 && strcasecmp(name + n - 5, ".anim") == 0;
}

void scanSDFiles() {
    fileCount = 0;
    File root = SD.open("/");
    if (!root) return;
    while (fileCount < MAX_FILES) {
        File entry = root.openNextFile();
        if (!entry) break;
        const char *name = entry.name();
        // Skip macOS "._foo.anim" resource-fork files
        if (!entry.isDirectory() && name[0] != '.' && isAnimName(name)) {
            strncpy(fileList[fileCount], name, sizeof(fileList[0]) - 1);
            fileList[fileCount][sizeof(fileList[0]) - 1] = '\0';
            fileCount++;
        }
        entry.close();
    }
    root.close();
}


bool openFile(int idx) {
    if (!haveDisplay || idx < 0 || idx >= fileCount) return false;
    if (animFile) animFile.close();
    String path = "/" + String(fileList[idx]);
    animFile = SD.open(path.c_str());
    if (!animFile) return false;

    // Header: version 1 = 11/14-panel Mk2/Mk3 file, version 2 = any display
    uint8_t hdr[ANIM_V2_HEADER];
    if (animFile.read(hdr, ANIM_V1_HEADER) != ANIM_V1_HEADER || memcmp(hdr, "ANIM", 4) != 0) {
        bleSendLine("ERROR: not a valid .anim file");
        animFile.close();
        return false;
    }
    bool fits;
    if (hdr[4] == 1) {
        fits = display.v1Compatible(hdr[5]);
        animHeaderSize = ANIM_V1_HEADER;
        if (!fits)
            bleSendLine("ERROR: file is for a " + String(hdr[5]) + "-panel Mk" +
                        String(hdr[5] == 14 ? 3 : 2) + " helmet, not \"" + display.name.c_str() + "\"");
    } else if (hdr[4] == 2) {
        if (animFile.read(hdr + ANIM_V1_HEADER, ANIM_V2_HEADER - ANIM_V1_HEADER) !=
            ANIM_V2_HEADER - ANIM_V1_HEADER) {
            bleSendLine("ERROR: .anim header is cut short");
            animFile.close();
            return false;
        }
        uint16_t hsize  = hdr[6] | (hdr[7] << 8);
        uint32_t pixels = hdr[8] | (hdr[9] << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
        uint32_t layout = hdr[12] | (hdr[13] << 8) | ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
        animHeaderSize = hsize;
        fits = hsize >= ANIM_V2_HEADER && pixels == display.totalPixels && layout == display.layoutId();
        if (!fits)
            bleSendLine("ERROR: file is for a different display (" + String(pixels) + " pixels, layout " +
                        String(layout, HEX) + "), not \"" + display.name.c_str() + "\"");
    } else {
        bleSendLine("ERROR: .anim version " + String(hdr[4]) + " is newer than this firmware");
        fits = false;
    }
    if (!fits) {
        animFile.close();
        return false;
    }
    totalFrames = (animFile.size() - animHeaderSize) / frameSize;
    if (totalFrames <= 0) {
        bleSendLine("ERROR: file has no complete frames");
        animFile.close();
        return false;
    }
    return animFile.seek(animHeaderSize);
}

bool readNextFrame() {
    if (!animFile) return false;
    uint8_t fhdr[FRAME_HDR_SIZE];
    if (animFile.read(fhdr, FRAME_HDR_SIZE) != FRAME_HDR_SIZE) return false;
    const size_t ledBytes = currentFrame.leds.size() * sizeof(LEDEntry);
    if ((size_t)animFile.read((uint8_t *)currentFrame.leds.data(), ledBytes) != ledBytes)
        return false;
    currentFrame.duration_ms = fhdr[0] | (fhdr[1] << 8);
    currentFrame.timing_mode = fhdr[2];
    return true;
}

bool seekFrame(int target) {
    if (!animFile || target < 0 || target >= totalFrames) return false;
    if (!animFile.seek(animHeaderSize + (long)target * frameSize)) return false;
    return readNextFrame();
}

void startFrame() {
    lastFrameTime = millis();
    triggerArmed  = false;   // wait for quiet before a SOUND frame can fire
}

void switchToFallback(const char *why) {
    useFallback = true;
    if (animFile) animFile.close();
    Serial.println(why);
    bleSendLine(why);
}

bool loadFile(int idx) {
    if (!openFile(idx)) return false;
    if (!readNextFrame()) {
        switchToFallback("ERROR: failed to read first frame — using fallback");
        return false;
    }
    fileIndex       = idx;
    currentFrameIdx = 0;
    useFallback     = false;
    startFrame();
    return true;
}

bool stepFrame(int delta) {
    if (useFallback || totalFrames <= 0) return false;
    currentFrameIdx = ((currentFrameIdx + delta) % totalFrames + totalFrames) % totalFrames;
    // Frames are stored back to back, so the common "next" step is a plain read.
    bool ok = (delta == 1 && currentFrameIdx != 0) ? readNextFrame()
                                                   : seekFrame(currentFrameIdx);
    if (!ok) {
        switchToFallback("ERROR: failed to read frame — switching to fallback");
        return false;
    }
    startFrame();
    return true;
}

// List the SD card's animations and load start.anim if it is there.
void startSD() {
    if (!sdReady) {
        Serial.println("SD not found — fallback only");
        return;
    }
    scanSDFiles();
    Serial.print(fileCount); Serial.println(" .anim files found");
    if (!haveDisplay) return;
    for (int i = 0; i < fileCount; i++) {
        if (strcasecmp(fileList[i], "start.anim") != 0) continue;
        if (loadFile(i)) {
            Serial.print("Auto-loaded: "); Serial.print(fileList[i]);
            Serial.print("  ("); Serial.print(totalFrames); Serial.println(" frames)");
        } else {
            Serial.println("start.anim could not be loaded — using fallback");
        }
        return;
    }
    Serial.println("start.anim not found — using fallback");
}

// ── Display config commands ───────────────────────────────────────────────────
void sendDisplayInfo() {
    bleSendLine("── Display ──────────────────────");
    if (!haveDisplay) {
        bleSendLine(displayProblem);
        if (display.name.empty()) { bleSendLine("─────────────────────────────────"); return; }
    }
    bleSendLine(String("Name:    ") + display.name.c_str());
    bleSendLine("Panels:  " + String((unsigned)display.panels.size()) + "  (" +
                String(display.totalPixels) + " pixels, layout " + String(display.layoutId(), HEX) + ")");
    for (const pf::Output &o : display.outputs) {
        String line = String("Output:  ") + o.name.c_str() + "  " + outputTypeName(o.type);
        if (o.type == pf::OUT_WS2812) line += " GP" + String(o.pin) + " " + o.colorOrder.c_str();
        line += "  " + String(o.length) + " LEDs, " + String((unsigned)o.place.size()) + " panels";
        bleSendLine(line);
    }
    bleSendLine("─────────────────────────────────");
}

// ── CLI ───────────────────────────────────────────────────────────────────────
void handleCommand(const char *cmd) {
    String raw = String(cmd);
    raw.trim();
    if (raw.length() == 0) return;

    String s = raw;
    s.toLowerCase();

    if (s == "display") {
        sendDisplayInfo();
        return;
    }
    if (s == "display clear") {
        LittleFS.remove(DISPLAY_FILE);
        bleSendLine("Display config removed from flash (a display.json on the SD card is copied "
                    "in again at boot). Restarting...");
        rebootAt = millis() + 1000;
        return;
    }
    if (s == "help") {
        bleSendLine("── Commands ─────────────────────");
        bleSendLine("  list          list .anim files on SD");
        bleSendLine("  load <n>      load file by index");
        bleSendLine("  fallback      show built-in fallback");
        bleSendLine("  play          resume animation");
        bleSendLine("  pause         pause animation");
        bleSendLine("  next          advance one frame");
        bleSendLine("  prev          go back one frame");
        bleSendLine("  bright <n>    brightness 0-255");
        bleSendLine("  status        show current state");
        bleSendLine("  power         show USB-C PD power info");
        bleSendLine("  reload        rescan SD card (and its display.json)");
        bleSendLine("  ident         show each panel's chain position");
        bleSendLine("  display       show the display config");
        bleSendLine("  display clear remove it from flash");
        bleSendLine("  help          show this message");
        bleSendLine("─────────────────────────────────");
        return;
    }
    if (s == "status") {
        bleSendLine("── Status ───────────────────────");
        bleSendLine("ProtoFace " FIRMWARE_VERSION);
        bleSendLine(haveDisplay ? String("Display: ") + display.name.c_str() + "  (" +
                                  String(display.totalPixels) + " pixels)"
                                : "Display: " + displayProblem);
        if (haveDisplay) {
            bleSendLine(useFallback ? String("Source: built-in NO SD face")
                                    : "Source: " + String(fileList[fileIndex]));
            bleSendLine(playing ? "State:  Playing" : "State:  Paused");
            if (!useFallback)
                bleSendLine("Frame:  " + String(currentFrameIdx + 1) + "/" + String(totalFrames));
        }
        bleSendLine(sdReady ? "SD:     Mounted" : "SD:     Not mounted");
        bleSendLine("Bright: " + String(brightness) + "/255  (power limit " +
                    String(lastPowerScale) + "/255)");
        bleSendLine(String("Power:  ") + (pdReady ? "PD " : "USB ") + String(pdVolts) +
                    "V " + String(availableAmps, 2) + "A");
        bleSendLine("Volume: " + String(readVolume()) + "/255");
        bleSendLine("─────────────────────────────────");
        return;
    }
    if (s == "power") {
        bleSendLine("── Power ────────────────────────");
        bleSendLine(pdReady ? "Source:    USB-C PD (HUSB238)"
                            : "Source:    USB / no PD chip");
        bleSendLine("Voltage:   " + String(pdVolts) + "V");
        bleSendLine("Available: " + String(availableAmps, 2) + "A");
        bleSendLine("LED budget: " + String((int)ledBudgetMA) + "mA  (full white = " +
                    String((int)(display.totalPixels * display.limits.maPerLedWhite)) + "mA)");
        bleSendLine("Frame dimming: " + String(lastPowerScale) + "/255");
        bleSendLine("─────────────────────────────────");
        return;
    }
    if (s == "bright" || s.startsWith("bright ")) {
        String arg = s.substring(6);
        arg.trim();
        if (arg.length() > 0) brightness = (uint8_t)constrain(arg.toInt(), 0, 255);
        bleSendLine("Brightness: " + String(brightness) + "/255");
        return;
    }
    if (s == "reload") {
        // Retry the mount if it failed at boot so a card can be re-seated or
        // wiring fixed without a power cycle. Each attempt re-drives CS.
        if (!sdReady) sdReady = SD.begin(SD_CS_PIN);
        if (sdReady && syncDisplayFromSD()) {
            bleSendLine("Restarting to use the new display...");
            rebootAt = millis() + 1000;
        } else if (sdReady) {
            // Rescanning can reorder the list, so drop back to the fallback
            // rather than keep playing a file whose index may now be stale.
            if (!useFallback) switchToFallback("Reloading — showing fallback");
            scanSDFiles();
            bleSendLine("SD rescan: " + String(fileCount) + " files found");
        } else {
            bleSendLine("SD still not mounted");
        }
        return;
    }
    if (s == "list") {
        if (!sdReady)       { bleSendLine("ERROR: SD not mounted"); return; }
        if (fileCount == 0) { bleSendLine("No .anim files on SD");  return; }
        bleSendLine("── .anim files ──────────────────");
        for (int i = 0; i < fileCount; i++) {
            String line = (!useFallback && i == fileIndex ? "> " : "  ");
            line += String(i) + ": " + String(fileList[i]);
            bleSendLine(line);
        }
        bleSendLine("─────────────────────────────────");
        return;
    }

    // Everything below needs a display
    if (!haveDisplay) {
        bleSendLine("No display set up: " + displayProblem);
        return;
    }
    if (s.startsWith("load ")) {
        String arg = s.substring(5);
        arg.trim();
        int idx = arg.toInt();
        if (arg.length() == 0 || !isDigit(arg[0]) || idx < 0 || idx >= fileCount) {
            bleSendLine("ERROR: invalid index — use 'list'"); return;
        }
        if (!loadFile(idx)) { bleSendLine("ERROR: could not load file"); return; }
        bleSendLine("Loaded: " + String(fileList[idx]) +
                    "  (" + String(totalFrames) + " frames)");
    }
    else if (s == "fallback") {
        switchToFallback("Showing built-in fallback");
    }
    else if (s == "play") {
        playing = true; startFrame();
        bleSendLine("Playing");
    }
    else if (s == "pause") {
        playing = false;
        bleSendLine("Paused");
    }
    else if (s == "next" || s == "prev") {
        if (useFallback) { bleSendLine("Fallback has one frame"); return; }
        if (stepFrame(s == "next" ? 1 : -1))
            bleSendLine("Frame " + String(currentFrameIdx + 1) + "/" + String(totalFrames));
    }
    else if (s == "ident") {
        identMode = !identMode;
        bleSendLine(identMode ? "Ident ON: each panel shows its position on its output's chain "
                                "(one colour per output, red dot = first LED). "
                                "'ident' again to go back."
                              : "Ident OFF");
    }
    else {
        bleSendLine("Unknown: '" + s + "' — type 'help'");
    }
}

// ── BLE callbacks ─────────────────────────────────────────────────────────────
// These run in BTstack's context: only copy data out, never touch SD/LEDs here.
int bleWriteCallback(uint16_t value_handle, uint8_t *buf, uint16_t len) {
    if (value_handle == rx_value_handle) {
        if (cmdPending) return 0;            // previous command not yet handled
        uint16_t n = min<uint16_t>(len, CMD_MAX - 1);
        memcpy(pendingCmd, buf, n);
        pendingCmd[n] = '\0';
        cmdPending = true;
    } else if (value_handle == tx_value_handle + 1) {
        // Client Characteristic Configuration Descriptor of the TX characteristic
        bool on = len > 0 && (buf[0] & 0x01);
        if (on && !ble_subscribed) ble_greet = true;
        ble_subscribed = on;
    }
    return 0;
}

void bleConnected(BLEStatus status, BLEDevice *device) {
    if (status == BLE_STATUS_OK) {
        conn_handle = device->getHandle();
        // Some apps never write the CCCD; treat the link as subscribed anyway.
        ble_subscribed = true;
        ble_greet      = true;
    }
}

void bleDisconnected(BLEDevice *device) {
    (void)device;
    conn_handle    = HCI_CON_HANDLE_INVALID;
    ble_subscribed = false;
    BTstack.startAdvertising();
}

void serviceBle() {
    if (ble_greet) {
        ble_greet = false;
        Serial.println("BLE connected");
        bleSendLine("Protogen connected. Type 'help'.");
    }
    if (cmdPending) {
        char cmd[CMD_MAX];
        memcpy(cmd, pendingCmd, CMD_MAX);
        cmdPending = false;
#if DEBUG_BLE
        Serial.print("BLE cmd: "); Serial.println(cmd);
#endif
        handleCommand(cmd);
    }
    bleFlush();
}


// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("ProtoFace " FIRMWARE_VERSION);
    analogReadResolution(12);

    if (!LittleFS.begin()) Serial.println("Flash file system not available");

    // Explicitly configure SPI0 pins so the mount does not depend on the
    // core's default mapping.  The SD card is read first: it may carry a new
    // display.json.
    SPI.setRX(SD_MISO_PIN);
    SPI.setTX(SD_MOSI_PIN);
    SPI.setSCK(SD_SCK_PIN);
    sdReady = SD.begin(SD_CS_PIN);
    Serial.println(sdReady ? "SD mounted" : "SD not mounted");
    syncDisplayFromSD();
    initDisplay();
    if (haveDisplay) {
        Serial.print("Display: "); Serial.print(display.name.c_str());
        Serial.print("  "); Serial.print(display.totalPixels); Serial.println(" pixels");
    } else {
        Serial.println(displayProblem);
    }

    // Negotiate USB-C PD (5V) and budget LED current to what is available.
    initPower();
    if (!pdReady && display.limits.maxMA > 0) ledBudgetMA = display.limits.maxMA;

    useFallback = true;
    playing     = true;
    startSD();

    BTstack.setBLEDeviceConnectedCallback(bleConnected);
    BTstack.setBLEDeviceDisconnectedCallback(bleDisconnected);
    BTstack.setGATTCharacteristicWrite(bleWriteCallback);
    BTstack.addGATTService(new UUID(NUS_SERVICE_UUID));
    rx_value_handle = BTstack.addGATTCharacteristicDynamic(
        new UUID(NUS_RX_UUID),
        ATT_PROPERTY_WRITE | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE,
        0);
    tx_value_handle = BTstack.addGATTCharacteristicDynamic(
        new UUID(NUS_TX_UUID),
        ATT_PROPERTY_NOTIFY,
        0);
    BTstack.setup("ProtoFace");
    BTstack.startAdvertising();
    Serial.println("BLE advertising as 'ProtoFace'");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    BTstack.loop();
    serviceBle();

    if (rebootAt && (long)(millis() - rebootAt) >= 0) {
        blankOutputs();
        rp2040.reboot();
    }
    if (!haveDisplay) { delay(5); return; }

    uint8_t vol = readVolume();

    const AnimFrame &f = useFallback ? fallbackFrame : currentFrame;
    if (identMode) pushIdent();
    else           pushFrame(f.leds.data(), vol);

    if (!playing || useFallback) { delay(5); return; }

    unsigned long held = millis() - lastFrameTime;
    bool advance;
    if (f.timing_mode == TIMING_SOUND) {
        if (vol < SOUND_TRIGGER_RELEASE) triggerArmed = true;
        advance = triggerArmed && vol >= SOUND_TRIGGER_LEVEL && held >= f.duration_ms;
    } else {
        advance = held >= f.duration_ms;
    }
    if (advance) stepFrame(1);
}
