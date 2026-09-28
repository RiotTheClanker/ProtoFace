#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_NeoPixel.h>
#include <BTstackLib.h>
#include <BluetoothLock.h>
#include <Wire.h>
#include <Adafruit_HUSB238.h>

// extern "C" must be at file scope, not inline with #include
extern "C" {
#include "ble/att_server.h"
}

#define FIRMWARE_VERSION "V6.1"

// ── Model / layout selection ──────────────────────────────────────────────────
//
//  Pick the helmet with PROTOGEN_LAYOUT (default 11), either here or with
//  -DPROTOGEN_LAYOUT=14 in platformio.ini:
//
//    PROTOGEN_LAYOUT 11  →  Mk2:  nose side=6 panels, plain side=5 panels (11 total)
//                           Left  chain: 384 LEDs  (panels 0-5  — nose side)
//                           Right chain: 320 LEDs  (panels 6-10 — plain side)
//
//    PROTOGEN_LAYOUT 14  →  Mk3:  7 panels per side (14 total)
//                           Left  chain: 448 LEDs  (panels 0-6 )
//                           Right chain: 448 LEDs  (panels 7-13)
//
//  The value must match the panel count stored in the .anim file header byte 5.
//  Panels in the LED data are in logical face order (eye → mouth → nose) on
//  both sides; see pushFrame() for how that maps to the physical wiring.
//
#ifndef PROTOGEN_LAYOUT
#  define PROTOGEN_LAYOUT 11
#endif

#define LEDS_PER_PANEL  64

#if PROTOGEN_LAYOUT == 14
    #define MODEL_NAME      "Mk3"
    #define PANELS_LEFT     7     // panels 0-6  (left chain)
    #define PANELS_RIGHT    7     // panels 7-13 (right chain)
#elif PROTOGEN_LAYOUT == 11
    #define MODEL_NAME      "Mk2"
    #define PANELS_LEFT     6     // panels 0-5  (nose side / left chain)
    #define PANELS_RIGHT    5     // panels 6-10 (plain side / right chain)
#else
    #error "PROTOGEN_LAYOUT must be 14 or 11"
#endif

#define LEDS_LEFT       (PANELS_LEFT  * LEDS_PER_PANEL)
#define LEDS_RIGHT      (PANELS_RIGHT * LEDS_PER_PANEL)
#define TOTAL_LEDS      (LEDS_LEFT + LEDS_RIGHT)

// ── Fallback animation ────────────────────────────────────────────────────────
// Shown when there is no SD card / start.anim.  Each model has its own frame
// in src/fallback/, exported by the Protogen AnimFile Maker ("Export .h" tab).
// A src/fallback_anim.h (gitignored) overrides it for local experiments.
#if __has_include("fallback_anim.h")
#  include "fallback_anim.h"
#  define FALLBACK_SOURCE "fallback_anim.h"
#elif PROTOGEN_LAYOUT == 14
#  include "fallback/mk3_fallback.h"
#  define FALLBACK_SOURCE "fallback/mk3_fallback.h"
#else
#  include "fallback/mk2_fallback.h"
#  define FALLBACK_SOURCE "fallback/mk2_fallback.h"
#endif
#if defined(FALLBACK_LAYOUT) && FALLBACK_LAYOUT != PROTOGEN_LAYOUT
#  error "The fallback header was exported for a different layout than PROTOGEN_LAYOUT — re-export it with the matching layout selected in the tool"
#endif
static_assert(sizeof(FALLBACK_LEDS) / sizeof(FALLBACK_LEDS[0]) == TOTAL_LEDS,
              "Fallback header LED count does not match PROTOGEN_LAYOUT — "
              "re-export it with the matching layout selected in the tool");

// The .anim reader copies raw 5-byte records straight into LEDEntry.
static_assert(sizeof(LEDEntry) == 5, "LEDEntry must be exactly 5 bytes");

// ── Debug ─────────────────────────────────────────────────────────────────────
#define DEBUG_BLE 0

// ── Pins ──────────────────────────────────────────────────────────────────────
#define LED_PIN_LEFT   2
#define LED_PIN_RIGHT  3
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

// ── NUS UUIDs ─────────────────────────────────────────────────────────────────
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ── .anim file layout ─────────────────────────────────────────────────────────
#define ANIM_HEADER_SIZE  8
#define FRAME_HDR_SIZE    4
#define FRAME_SIZE        (FRAME_HDR_SIZE + TOTAL_LEDS * (long)sizeof(LEDEntry))

// ── LED strips ────────────────────────────────────────────────────────────────
// Left  = first side  (LEDS_LEFT  LEDs, GP2)
// Right = second side (LEDS_RIGHT LEDs, GP3)
Adafruit_NeoPixel stripL(LEDS_LEFT,  LED_PIN_LEFT,  NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripR(LEDS_RIGHT, LED_PIN_RIGHT, NEO_GRB + NEO_KHZ800);

// ── Playback state ────────────────────────────────────────────────────────────
AnimFrame     currentFrame;
bool          playing        = true;
bool          sdReady        = false;
unsigned long lastFrameTime  = 0;
bool          triggerArmed   = true;
uint8_t       brightness     = 255;   // user brightness ('bright' command)

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
File animFile;

AnimFrame fallbackFrame;
bool      useFallback = true;

// ── BLE state ─────────────────────────────────────────────────────────────────
// BTstack callbacks run outside loop(), so they only hand data over through
// these buffers; commands are executed and replies sent from loop().
static volatile hci_con_handle_t conn_handle     = HCI_CON_HANDLE_INVALID;
static uint16_t                  tx_value_handle = 0;
static uint16_t                  rx_value_handle = 0;
static volatile bool             ble_subscribed  = false;
static volatile bool             ble_greet       = false;

#define CMD_MAX 64
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

// ── Push frame ────────────────────────────────────────────────────────────────
// LED data is stored in logical face order: left chain eye→mouth→nose, right
// chain eye→mouth(→nose).  Hardware constraint: data flows right-to-left, so the
// left chain's physical wiring is reversed (nose→mouth→eye).  Panels are
// therefore written in reverse order to the left strip so the image appears
// correctly on the face.  Right strip wiring matches logical order.
//
// Scratch buffers hold one frame's final colours (after sound reaction) in
// logical order, so the frame's current draw can be measured before writing.
static uint8_t frameR[TOTAL_LEDS];
static uint8_t frameG[TOTAL_LEDS];
static uint8_t frameB[TOTAL_LEDS];

void pushFrame(const LEDEntry *leds, uint8_t vol) {
    // ── Pass 1: resolve final colours (sound + user brightness), sum levels ───
    uint32_t sumCh = 0;
    for (int i = 0; i < TOTAL_LEDS; i++) {
        uint8_t r, g, b;
        applySound(leds[i], vol, r, g, b);
        r = (uint16_t)r * brightness / 255;
        g = (uint16_t)g * brightness / 255;
        b = (uint16_t)b * brightness / 255;
        frameR[i] = r; frameG[i] = g; frameB[i] = b;
        sumCh += (uint32_t)r + g + b;
    }

    // ── Estimate this frame's current draw ────────────────────────────────────
    //   dynamic   = (sum of channel levels / 255) × per-channel full current
    //   quiescent = fixed per-LED controller draw (cannot be dimmed away)
    float dynMA       = (float)sumCh / 255.0f * (LED_FULL_WHITE_MA / 3.0f);
    float quiescentMA = (float)TOTAL_LEDS * LED_QUIESCENT_MA;

    // ── Only dim if this frame would exceed the power budget ──────────────────
    uint16_t scale = 255;
    if (ledBudgetMA > 0.0f && (dynMA + quiescentMA) > ledBudgetMA && dynMA > 0.0f) {
        float avail = ledBudgetMA - quiescentMA;   // current left for the colour part
        if (avail < 0) avail = 0;
        int s = (int)(avail / dynMA * 255.0f);
        if (s < LED_MIN_BRIGHTNESS) s = LED_MIN_BRIGHTNESS;
        if (s > 255) s = 255;
        scale = (uint16_t)s;
    }
    lastPowerScale = (uint8_t)scale;

    // ── Pass 2: write to strips with the scale applied ────────────────────────
    // Left strip: reverse panel order to correct for reversed physical wiring.
    for (int p = 0; p < PANELS_LEFT; p++) {
        int srcPanel = PANELS_LEFT - 1 - p;
        for (int j = 0; j < LEDS_PER_PANEL; j++) {
            int src = srcPanel * LEDS_PER_PANEL + j;
            stripL.setPixelColor(p * LEDS_PER_PANEL + j,
                                 (uint16_t)frameR[src] * scale / 255,
                                 (uint16_t)frameG[src] * scale / 255,
                                 (uint16_t)frameB[src] * scale / 255);
        }
    }
    // Right strip: logical order matches physical wiring.
    for (int i = 0; i < LEDS_RIGHT; i++) {
        int src = LEDS_LEFT + i;
        stripR.setPixelColor(i,
                             (uint16_t)frameR[src] * scale / 255,
                             (uint16_t)frameG[src] * scale / 255,
                             (uint16_t)frameB[src] * scale / 255);
    }
    stripL.show();
    stripR.show();
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
    if (idx < 0 || idx >= fileCount) return false;
    if (animFile) animFile.close();
    String path = "/" + String(fileList[idx]);
    animFile = SD.open(path.c_str());
    if (!animFile) return false;

    // Validate header: magic + layout panel count must match compile-time layout
    uint8_t hdr[ANIM_HEADER_SIZE];
    if (animFile.read(hdr, ANIM_HEADER_SIZE) != ANIM_HEADER_SIZE ||
        memcmp(hdr, "ANIM", 4) != 0) {
        bleSendLine("ERROR: not a valid .anim file");
        animFile.close();
        return false;
    }
    uint8_t filePanels = hdr[5];
    if (filePanels != PROTOGEN_LAYOUT) {
        bleSendLine("ERROR: file layout " + String(filePanels) +
                    " != build layout " + String(PROTOGEN_LAYOUT));
        animFile.close();
        return false;
    }
    totalFrames = (animFile.size() - ANIM_HEADER_SIZE) / FRAME_SIZE;
    if (totalFrames <= 0) {
        bleSendLine("ERROR: file has no complete frames");
        animFile.close();
        return false;
    }
    return true;
}

bool readNextFrame() {
    if (!animFile) return false;
    uint8_t fhdr[FRAME_HDR_SIZE];
    if (animFile.read(fhdr, FRAME_HDR_SIZE) != FRAME_HDR_SIZE) return false;
    const size_t ledBytes = sizeof(currentFrame.leds);
    if ((size_t)animFile.read((uint8_t *)currentFrame.leds, ledBytes) != ledBytes)
        return false;
    currentFrame.duration_ms = fhdr[0] | (fhdr[1] << 8);
    currentFrame.timing_mode = fhdr[2];
    return true;
}

bool seekFrame(int target) {
    if (!animFile || target < 0 || target >= totalFrames) return false;
    if (!animFile.seek(ANIM_HEADER_SIZE + (long)target * FRAME_SIZE)) return false;
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

// ── CLI ───────────────────────────────────────────────────────────────────────
void handleCommand(const char *cmd) {
    String s = String(cmd);
    s.trim();
    s.toLowerCase();

    if (s.length() == 0) return;

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
    }
    else if (s.startsWith("load ")) {
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
    else if (s.startsWith("bright")) {
        String arg = s.substring(6);
        arg.trim();
        if (arg.length() > 0) {
            brightness = (uint8_t)constrain(arg.toInt(), 0, 255);
        }
        bleSendLine("Brightness: " + String(brightness) + "/255");
    }
    else if (s == "status") {
        bleSendLine("── Status ───────────────────────");
        bleSendLine("ProtoFace " FIRMWARE_VERSION "  (" MODEL_NAME ")");
        bleSendLine(useFallback ? String("Source: FALLBACK (" FALLBACK_SOURCE ")")
                                : "Source: " + String(fileList[fileIndex]));
        bleSendLine(playing ? "State:  Playing" : "State:  Paused");
        if (!useFallback)
            bleSendLine("Frame:  " + String(currentFrameIdx + 1) + "/" + String(totalFrames));
        bleSendLine(sdReady ? "SD:     Mounted" : "SD:     Not mounted");
        bleSendLine("Layout: " + String(PROTOGEN_LAYOUT) + "-panel  (L=" + String(LEDS_LEFT) +
                    " R=" + String(LEDS_RIGHT) + " LEDs)");
        bleSendLine("Bright: " + String(brightness) + "/255  (power limit " +
                    String(lastPowerScale) + "/255)");
        bleSendLine(String("Power:  ") + (pdReady ? "PD " : "USB ") + String(pdVolts) +
                    "V " + String(availableAmps, 2) + "A");
        bleSendLine("Volume: " + String(readVolume()) + "/255");
        bleSendLine("─────────────────────────────────");
    }
    else if (s == "power") {
        bleSendLine("── Power ────────────────────────");
        bleSendLine(pdReady ? "Source:    USB-C PD (HUSB238)"
                            : "Source:    USB / no PD chip");
        bleSendLine("Voltage:   " + String(pdVolts) + "V");
        bleSendLine("Available: " + String(availableAmps, 2) + "A");
        bleSendLine("LED budget: " + String((int)ledBudgetMA) + "mA  (full white = " +
                    String((int)(TOTAL_LEDS * LED_FULL_WHITE_MA)) + "mA)");
        bleSendLine("Frame dimming: " + String(lastPowerScale) + "/255");
        bleSendLine("─────────────────────────────────");
    }
    else if (s == "reload") {
        // Retry the mount if it failed at boot so a card can be re-seated or
        // wiring fixed without a power cycle. Each attempt re-drives CS.
        if (!sdReady) sdReady = SD.begin(SD_CS_PIN);
        if (sdReady) {
            // Rescanning can reorder the list, so drop back to the fallback
            // rather than keep playing a file whose index may now be stale.
            if (!useFallback) switchToFallback("Reloading — showing fallback");
            scanSDFiles();
            bleSendLine("SD rescan: " + String(fileCount) + " files found");
        } else {
            bleSendLine("SD still not mounted");
        }
    }
    else if (s == "help") {
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
        bleSendLine("  reload        rescan SD card");
        bleSendLine("  help          show this message");
        bleSendLine("─────────────────────────────────");
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

    Serial.println("ProtoFace " FIRMWARE_VERSION "  (" MODEL_NAME ")");
    Serial.print("Build layout: ");
    Serial.print(PROTOGEN_LAYOUT);
    Serial.print("-panel  TOTAL_LEDS=");
    Serial.print(TOTAL_LEDS);
    Serial.print("  left=");
    Serial.print(LEDS_LEFT);
    Serial.print("  right=");
    Serial.println(LEDS_RIGHT);

    analogReadResolution(12);

    stripL.begin(); stripR.begin();
    stripL.clear(); stripL.show();
    stripR.clear(); stripR.show();

    // Negotiate USB-C PD (5V) and budget LED current to what is available.
    initPower();

    fallbackFrame = makeFallbackFrame();
    useFallback   = true;
    playing       = true;
    Serial.println("Fallback active (" FALLBACK_SOURCE ")");

    // Explicitly configure SPI0 pins so the mount does not depend on the
    // core's default mapping. Left LED/right LED are on GP2/GP3; SD uses SPI0.
    SPI.setRX(SD_MISO_PIN);
    SPI.setTX(SD_MOSI_PIN);
    SPI.setSCK(SD_SCK_PIN);

    sdReady = SD.begin(SD_CS_PIN);
    if (sdReady) {
        Serial.println("SD mounted");
        scanSDFiles();
        Serial.print(fileCount); Serial.println(" .anim files found");

        int startIndex = -1;
        for (int i = 0; i < fileCount; i++) {
            if (strcasecmp(fileList[i], "start.anim") == 0) {
                startIndex = i;
                break;
            }
        }
        if (startIndex == -1) {
            Serial.println("start.anim not found — using fallback");
        } else if (loadFile(startIndex)) {
            Serial.print("Auto-loaded: "); Serial.print(fileList[startIndex]);
            Serial.print("  ("); Serial.print(totalFrames); Serial.println(" frames)");
        } else {
            Serial.println("start.anim could not be loaded — using fallback");
        }
    } else {
        Serial.println("SD not found — fallback only");
    }

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

    uint8_t vol = readVolume();

    const AnimFrame &f = useFallback ? fallbackFrame : currentFrame;
    pushFrame(f.leds, vol);

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
