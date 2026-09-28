#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_NeoPixel.h>
#include <BTstackLib.h>
#include <BluetoothLock.h>

// extern "C" must be at file scope, not inline with #include
extern "C" {
#include "ble/att_server.h"
}

// ── Layout selection ──────────────────────────────────────────────────────────
//
//  Set PROTOGEN_LAYOUT here (or pass -DPROTOGEN_LAYOUT=14 in platformio.ini):
//
//    PROTOGEN_LAYOUT 14  →  Original: 7 panels per side (7+7 = 14 total)
//                           Left  chain: 448 LEDs  (panels 0-6 )
//                           Right chain: 448 LEDs  (panels 7-13)
//
//    PROTOGEN_LAYOUT 11  →  New:  nose side=6 panels, plain side=5 panels (6+5 = 11 total)
//                           Left  chain: 384 LEDs  (panels 0-5  — nose side)
//                           Right chain: 320 LEDs  (panels 6-10 — plain side)
//
//  The value must match the panel count stored in the .anim file header byte 5,
//  and src/fallback_anim.h (if present) must be exported for the same layout —
//  the build stops with an error if it isn't.
//
#ifndef PROTOGEN_LAYOUT
#  define PROTOGEN_LAYOUT 11
#endif

#if PROTOGEN_LAYOUT == 14
    #define LEDS_LEFT       448   // panels 0-6  (left chain)
    #define LEDS_RIGHT      448   // panels 7-13 (right chain)
#elif PROTOGEN_LAYOUT == 11
    #define LEDS_LEFT       384   // panels 0-5  (nose side)
    #define LEDS_RIGHT      320   // panels 6-10 (plain side)
#else
    #error "PROTOGEN_LAYOUT must be 14 or 11"
#endif

#define TOTAL_LEDS      (LEDS_LEFT + LEDS_RIGHT)

// ── Fallback animation ────────────────────────────────────────────────────────
// src/fallback_anim.h is exported by the Protogen AnimFile Maker ("Export .h"
// tab).  It is optional: without it a plain warm-orange frame is used.
#if __has_include("fallback_anim.h")
#  include "fallback_anim.h"
#  define HAVE_FALLBACK_HEADER 1
#  if defined(FALLBACK_LAYOUT) && FALLBACK_LAYOUT != PROTOGEN_LAYOUT
#    error "fallback_anim.h was exported for a different layout than PROTOGEN_LAYOUT — re-export it with the matching layout selected in the tool"
#  endif
static_assert(sizeof(FALLBACK_LEDS) / sizeof(FALLBACK_LEDS[0]) == TOTAL_LEDS,
              "fallback_anim.h LED count does not match PROTOGEN_LAYOUT — "
              "re-export it with the matching layout selected in the tool");
#else
#  define HAVE_FALLBACK_HEADER 0
struct LEDEntry {
    uint8_t r, g, b;
    uint8_t sound_mode;
    uint8_t param;
};

struct AnimFrame {
    uint16_t duration_ms;
    uint8_t  timing_mode;
    LEDEntry leds[TOTAL_LEDS];
};

static AnimFrame makeFallbackFrame() {
    AnimFrame f;
    f.duration_ms = 1000;
    f.timing_mode = 0;
    for (int i = 0; i < TOTAL_LEDS; i++)
        f.leds[i] = {40, 12, 0, 0, 0};   // dim warm orange, static
    return f;
}
#endif

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
#define SOUND_AO_PIN   26

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

// Microphone reading: 1 = peak-to-peak (for raw analog mic modules whose
// output idles at mid-rail, e.g. MAX4466 / MAX9814), 0 = raw peak level
// (for modules that already output an envelope).
#define MIC_PEAK_TO_PEAK  1
#define MIC_SAMPLES       32

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
uint8_t       brightness     = 255;

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

// ── ADC ───────────────────────────────────────────────────────────────────────
uint8_t readVolume() {
    uint16_t lo = 4095, hi = 0;
    for (int i = 0; i < MIC_SAMPLES; i++) {
        uint16_t v = analogRead(SOUND_AO_PIN);
        if (v > hi) hi = v;
        if (v < lo) lo = v;
        delayMicroseconds(25);
    }
#if MIC_PEAK_TO_PEAK
    uint16_t level = (hi - lo) * 2;          // swing is at most half the range
    return (uint8_t)min<uint16_t>(level >> 4, 255);
#else
    return (uint8_t)(hi >> 4);
#endif
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
// LED indices 0 .. LEDS_LEFT-1          → left  strip (first side)
// LED indices LEDS_LEFT .. TOTAL_LEDS-1 → right strip (second side)
void pushFrame(const LEDEntry *leds, uint8_t vol) {
    for (int i = 0; i < TOTAL_LEDS; i++) {
        uint8_t r, g, b;
        applySound(leds[i], vol, r, g, b);
        if (i < LEDS_LEFT)
            stripL.setPixelColor(i, r, g, b);
        else
            stripR.setPixelColor(i - LEDS_LEFT, r, g, b);
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
            stripL.setBrightness(brightness);
            stripR.setBrightness(brightness);
        }
        bleSendLine("Brightness: " + String(brightness) + "/255");
    }
    else if (s == "status") {
        bleSendLine("── Status ───────────────────────");
        bleSendLine(useFallback ? String(HAVE_FALLBACK_HEADER ? "Source: FALLBACK (fallback_anim.h)"
                                                              : "Source: FALLBACK (plain orange)")
                                : "Source: " + String(fileList[fileIndex]));
        bleSendLine(playing ? "State:  Playing" : "State:  Paused");
        if (!useFallback)
            bleSendLine("Frame:  " + String(currentFrameIdx + 1) + "/" + String(totalFrames));
        bleSendLine(sdReady ? "SD:     Mounted" : "SD:     Not mounted");
        bleSendLine("Layout: " + String(PROTOGEN_LAYOUT) + "-panel  (L=" + String(LEDS_LEFT) +
                    " R=" + String(LEDS_RIGHT) + " LEDs)");
        bleSendLine("Bright: " + String(brightness) + "/255");
        bleSendLine("Volume: " + String(readVolume()) + "/255");
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
    stripL.setBrightness(brightness);
    stripR.setBrightness(brightness);
    stripL.clear(); stripL.show();
    stripR.clear(); stripR.show();

    fallbackFrame = makeFallbackFrame();
    useFallback   = true;
    playing       = true;
    Serial.println(HAVE_FALLBACK_HEADER ? "Fallback active (fallback_anim.h)"
                                        : "Fallback active (no fallback_anim.h — plain orange)");

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
