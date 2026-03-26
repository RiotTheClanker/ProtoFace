#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_NeoPixel.h>
#include <BTstackLib.h>

// extern "C" must be at file scope, not inline with #include
extern "C" {
#include "ble/att_server.h"
}

#include "fallback_anim.h"

// ── Layout selection ──────────────────────────────────────────────────────────
//
//  Define exactly ONE of these before building (or pass via -D flag):
//
//    PROTOGEN_LAYOUT 14  →  Original: 7 panels per side (7+7 = 14 total)
//                           Left  chain: 448 LEDs  (panels 0-6 )
//                           Right chain: 448 LEDs  (panels 7-13)
//
//    PROTOGEN_LAYOUT 11  →  New:  nose side=6 panels, plain side=5 panels (6+5 = 11 total)
//                           Left  chain: 384 LEDs  (panels 0-5  — nose side)
//                           Right chain: 320 LEDs  (panels 6-10 — plain side)
//
//  The value must match the panel count stored in the .anim file header byte 5.
//  The fallback_anim.h must be generated for the same layout.
//
#ifndef PROTOGEN_LAYOUT
#  define PROTOGEN_LAYOUT 11   // ← change to 11 for the new 11-panel build
#endif

#define LEDS_PER_PANEL  64    // all layouts use 64-LED panels

#if PROTOGEN_LAYOUT == 14
    #define LEDS_LEFT       448   // panels 0-6  (left chain)
    #define LEDS_RIGHT      448   // panels 7-13 (right chain)
    #define TOTAL_LEDS      896
    #define PANELS_LEFT     7
    #define PANELS_RIGHT    7
    // Panel indices (0-based in the flat LED array)
    #define PANEL_EYE_L     0
    #define PANEL_EYE_R     1
    // Mouth panels: 2, 3, 4, 5  (4 panels)
    #define PANEL_NOSE      6
    // Mirror side: identical panel layout starting at panel 7
    #define PANEL_MIRROR_OFFSET 7
#elif PROTOGEN_LAYOUT == 11
    #define LEDS_LEFT       384   // panels 0-5  (nose side / left chain)
    #define LEDS_RIGHT      320   // panels 6-10 (plain side / right chain)
    #define TOTAL_LEDS      704
    #define PANELS_LEFT     6
    #define PANELS_RIGHT    5
    // Left chain logical order (eye→mouth→nose) — physical wiring is reversed:
    //   data enters at nose, flows mouth→eye; panels are written reversed at output
    #define PANEL_EYE_L     0
    #define PANEL_EYE_R     1
    // Mouth panels (left chain): 2, 3, 4  (3 panels)
    #define PANEL_NOSE      5
    // Right chain panel indices (eye→mouth, no nose)
    #define PANEL_EYE_L2    6
    #define PANEL_EYE_R2    7
    // Mouth panels (right chain): 8, 9, 10  (3 panels)
#else
    #error "PROTOGEN_LAYOUT must be 14 or 11"
#endif

#define LEDS_PER_CHAIN  LEDS_LEFT   // left (nose-side) chain length

// ── Debug ─────────────────────────────────────────────────────────────────────
#define DEBUG_BLE 0

// ── Pins ──────────────────────────────────────────────────────────────────────
#define LED_PIN_LEFT   2
#define LED_PIN_RIGHT  3
#define SD_CS_PIN      17
#define SOUND_AO_PIN   26

// ── Sound / timing modes ──────────────────────────────────────────────────────
#define SOUND_STATIC  0
#define SOUND_SNAP    1
#define SOUND_LINEAR  2
#define TIMING_TIMED  0
#define TIMING_SOUND  1

// ── NUS UUIDs ─────────────────────────────────────────────────────────────────
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_CHAR_ID    1
#define NUS_TX_CHAR_ID    2

// ── LED strips ────────────────────────────────────────────────────────────────
// Left  = nose side  (LEDS_LEFT  LEDs)
// Right = plain side (LEDS_RIGHT LEDs)
Adafruit_NeoPixel stripL(LEDS_LEFT,  LED_PIN_LEFT,  NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripR(LEDS_RIGHT, LED_PIN_RIGHT, NEO_GRB + NEO_KHZ800);

// ── Playback state ────────────────────────────────────────────────────────────
struct Frame {
    uint16_t duration_ms;
    uint8_t  timing_mode;
    LEDEntry leds[TOTAL_LEDS];
};

Frame         currentFrame;
bool          frameLoaded    = false;
bool          playing        = true;
bool          sdReady        = false;
unsigned long lastFrameTime  = 0;

#define MAX_FILES 32
char fileList[MAX_FILES][64];
int  fileCount      = 0;
int  fileIndex      = 0;
int  currentFrameIdx = 0;
int  totalFrames    = 0;
File animFile;

AnimFrame fallbackFrame;
bool      useFallback = false;

// ── BLE state ─────────────────────────────────────────────────────────────────
static hci_con_handle_t conn_handle     = HCI_CON_HANDLE_INVALID;
static uint16_t         tx_value_handle = 0;
static uint16_t         rx_value_handle = 0;
static bool             ble_subscribed  = false;

// ── BLE helpers ───────────────────────────────────────────────────────────────
void bleSend(const char *msg) {
    if (!ble_subscribed || conn_handle == HCI_CON_HANDLE_INVALID) return;
    att_server_notify(conn_handle, tx_value_handle,
                      (uint8_t*)msg, strlen(msg));
}

void bleSendLine(const char *msg) {
    String s = String(msg) + "\r\n";
    bleSend(s.c_str());
}

// ── ADC ───────────────────────────────────────────────────────────────────────
uint8_t readVolume() {
    uint16_t peak = 0;
    for (int i = 0; i < 8; i++) {
        uint16_t v = analogRead(SOUND_AO_PIN);
        if (v > peak) peak = v;
        delayMicroseconds(100);
    }
    return (uint8_t)(peak >> 4);
}

// ── Sound reaction ────────────────────────────────────────────────────────────
void applySound(const LEDEntry &led, uint8_t vol,
                uint8_t &r, uint8_t &g, uint8_t &b) {
    switch (led.sound_mode) {
        case SOUND_STATIC:
            r = led.r; g = led.g; b = led.b; break;
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
    }
}

// ── Push frame ────────────────────────────────────────────────────────────────
// LED data is stored in logical face order: left chain eye→mouth→nose, right chain eye→mouth.
// Hardware constraint: data flows right-to-left, so the left chain's physical
// wiring is reversed (nose→mouth→eye). Panels are therefore written in reverse
// order to the left strip so the image appears correctly on the face.
// Right strip wiring matches logical order — no reversal needed.
void pushFrame(const LEDEntry *leds, uint8_t vol) {
    // Left strip: write panels in reverse order to correct for reversed wiring
    for (int p = 0; p < PANELS_LEFT; p++) {
        int srcPanel = PANELS_LEFT - 1 - p;
        for (int j = 0; j < LEDS_PER_PANEL; j++) {
            uint8_t r, g, b;
            applySound(leds[srcPanel * LEDS_PER_PANEL + j], vol, r, g, b);
            stripL.setPixelColor(p * LEDS_PER_PANEL + j, stripL.Color(r, g, b));
        }
    }
    // Right strip: logical order matches physical wiring
    for (int i = 0; i < LEDS_RIGHT; i++) {
        uint8_t r, g, b;
        applySound(leds[LEDS_LEFT + i], vol, r, g, b);
        stripR.setPixelColor(i, stripR.Color(r, g, b));
    }
    stripL.show();
    stripR.show();
}

// ── SD helpers ────────────────────────────────────────────────────────────────
void scanSDFiles() {
    fileCount = 0;
    File root = SD.open("/");
    if (!root) return;
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) {
            String name = entry.name();
            if (name.endsWith(".anim") || name.endsWith(".ANIM")) {
                if (fileCount < MAX_FILES) {
                    name.toCharArray(fileList[fileCount], 64);
                    fileCount++;
                }
            }
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
    uint8_t hdr[8];
    if (animFile.read(hdr, 8) != 8) { animFile.close(); return false; }
    if (memcmp(hdr, "ANIM", 4) != 0) { animFile.close(); return false; }
    uint8_t filePanels = hdr[5];
    if (filePanels != PROTOGEN_LAYOUT) {
        String msg = "ERROR: file layout ";
        msg += String(filePanels);
        msg += " != build layout ";
        msg += String(PROTOGEN_LAYOUT);
        bleSendLine(msg.c_str());
        animFile.close();
        return false;
    }

    useFallback = false;
    return true;
}

bool readNextFrame() {
    uint8_t fhdr[4];
    if (animFile.read(fhdr, 4) != 4) return false;
    currentFrame.duration_ms = fhdr[0] | (fhdr[1] << 8);
    currentFrame.timing_mode = fhdr[2];
    for (int i = 0; i < TOTAL_LEDS; i++) {
        uint8_t e[5];
        if (animFile.read(e, 5) != 5) return false;
        currentFrame.leds[i] = {e[0], e[1], e[2], e[3], e[4]};
    }
    frameLoaded = true;
    return true;
}

bool rewindFile() {
    if (!animFile) return false;
    animFile.seek(8);
    return readNextFrame();
}

bool seekFrame(int target) {
    if (!animFile) return false;
    animFile.seek(8);
    for (int i = 0; i < target; i++) {
        if (!animFile.seek(animFile.position() + 4 + TOTAL_LEDS * 5))
            return false;
    }
    return readNextFrame();
}

void countFrames() {
    if (!animFile) return;
    long saved = animFile.position();
    animFile.seek(8);
    totalFrames = 0;
    while (true) {
        long next = animFile.position() + 4 + TOTAL_LEDS * 5;
        if (next > (long)animFile.size()) break;
        animFile.seek(next);
        totalFrames++;
    }
    animFile.seek(saved);
}

// ── CLI ───────────────────────────────────────────────────────────────────────
void handleCommand(const char *cmd) {
    String s = String(cmd);
    s.trim();
    s.toLowerCase();

    if (s == "list") {
        if (!sdReady)       { bleSendLine("ERROR: SD not mounted"); return; }
        if (fileCount == 0) { bleSendLine("No .anim files on SD");  return; }
        bleSendLine("── .anim files ──────────────────");
        for (int i = 0; i < fileCount; i++) {
            String line = (i == fileIndex ? "> " : "  ");
            line += String(i) + ": " + String(fileList[i]);
            bleSendLine(line.c_str());
        }
        bleSendLine("─────────────────────────────────");
    }
    else if (s.startsWith("load ")) {
        int idx = s.substring(5).toInt();
        if (idx < 0 || idx >= fileCount) {
            bleSendLine("ERROR: invalid index — use 'list'"); return;
        }
        if (!openFile(idx)) { bleSendLine("ERROR: could not open file"); return; }
        fileIndex = idx; currentFrameIdx = 0;
        countFrames(); readNextFrame();
        if (!frameLoaded) {
            useFallback = true;
            bleSendLine("ERROR: failed to read first frame — using fallback");
            return;
        }
        lastFrameTime = millis();
        String msg = "Loaded: " + String(fileList[idx])
                   + "  (" + String(totalFrames) + " frames)";
        bleSendLine(msg.c_str());
    }
    else if (s == "play") {
        playing = true; lastFrameTime = millis();
        bleSendLine("Playing");
    }
    else if (s == "pause") {
        playing = false;
        bleSendLine("Paused");
    }
    else if (s == "next") {
        if (useFallback) { bleSendLine("Fallback has one frame"); return; }
        currentFrameIdx = (currentFrameIdx + 1) % totalFrames;
        if (!seekFrame(currentFrameIdx)) {
            bleSendLine("ERROR: failed to seek frame — switching to fallback");
            useFallback = true;
            return;
        }
        lastFrameTime = millis();
        bleSendLine(("Frame " + String(currentFrameIdx+1) + "/" + String(totalFrames)).c_str());
    }
    else if (s == "prev") {
        if (useFallback) { bleSendLine("Fallback has one frame"); return; }
        currentFrameIdx = (currentFrameIdx - 1 + totalFrames) % totalFrames;
        if (!seekFrame(currentFrameIdx)) {
            bleSendLine("ERROR: failed to seek frame — switching to fallback");
            useFallback = true;
            return;
        }
        lastFrameTime = millis();
        bleSendLine(("Frame " + String(currentFrameIdx+1) + "/" + String(totalFrames)).c_str());
    }
    else if (s == "status") {
        bleSendLine("── Status ───────────────────────");
        bleSendLine(useFallback ? "Source: FALLBACK (built-in)"
                                : ("Source: " + String(fileList[fileIndex])).c_str());
        bleSendLine(playing ? "State:  Playing" : "State:  Paused");
        if (!useFallback)
            bleSendLine(("Frame:  " + String(currentFrameIdx+1) + "/" + String(totalFrames)).c_str());
        bleSendLine(sdReady ? "SD:     Mounted" : "SD:     Not mounted");
        bleSendLine(("Layout: " + String(PROTOGEN_LAYOUT) + "-panel  (" + String(TOTAL_LEDS) + " LEDs)").c_str());
        bleSendLine("─────────────────────────────────");
    }
    else if (s == "reload") {
        scanSDFiles();
        bleSendLine(("SD rescan: " + String(fileCount) + " files found").c_str());
    }
    else if (s == "help") {
        bleSendLine("── Commands ─────────────────────");
        bleSendLine("  list          list .anim files on SD");
        bleSendLine("  load <n>      load file by index");
        bleSendLine("  play          resume animation");
        bleSendLine("  pause         pause animation");
        bleSendLine("  next          advance one frame");
        bleSendLine("  prev          go back one frame");
        bleSendLine("  status        show current state");
        bleSendLine("  reload        rescan SD card");
        bleSendLine("  help          show this message");
        bleSendLine("─────────────────────────────────");
    }
    else {
        bleSendLine(("Unknown: '" + String(cmd) + "' — type 'help'").c_str());
    }
}

// ── BLE callbacks ─────────────────────────────────────────────────────────────
int bleWriteCallback(uint16_t characteristic_id, uint8_t *buf, uint16_t len) {
    if (characteristic_id == rx_value_handle) {
        char cmd[64] = {0};
        memcpy(cmd, buf, min((int)len, 63));
        handleCommand(cmd);
    }
    return 0;
}

void bleNotifyCallback(BLEDevice *device, uint16_t value_handle,
                       uint8_t *value, uint16_t length) {
    ble_subscribed = (length > 0 && value[0] != 0);
    if (ble_subscribed)
        bleSendLine("Protogen connected. Type 'help'.");
}

void bleConnected(BLEStatus status, BLEDevice *device) {
    if (status == BLE_STATUS_OK) {
        conn_handle = device->getHandle();
        Serial.println("BLE connected");
        ble_subscribed = true;  // Force subscribed for apps that don't enable notifications
        bleSendLine("Protogen connected. Type 'help'.");
    }
}

void bleDisconnected(BLEDevice *device) {
    conn_handle    = HCI_CON_HANDLE_INVALID;
    ble_subscribed = false;
    Serial.println("BLE disconnected");
    BTstack.startAdvertising();
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
    stripL.clear(); stripL.show();
    stripR.clear(); stripR.show();

    fallbackFrame = makeFallbackFrame();
    useFallback   = true;
    frameLoaded   = true;
    playing       = true;
    Serial.println("Fallback active");

    sdReady = SD.begin(SD_CS_PIN);
    if (sdReady) {
        Serial.println("SD mounted");
        scanSDFiles();
        Serial.print(fileCount); Serial.println(" .anim files found");

        int startIndex = -1;
        for (int i = 0; i < fileCount; i++) {
            if (strcmp(fileList[i], "start.anim") == 0) {
                startIndex = i;
                break;
            }
        }
        if (startIndex != -1 && openFile(startIndex)) {
            fileIndex = startIndex;
            currentFrameIdx = 0;
            countFrames();
            readNextFrame();
            if (!frameLoaded) {
                useFallback = true;
                Serial.println("Failed to read first frame — using fallback");
            } else {
                lastFrameTime = millis();
                Serial.print("Auto-loaded: "); Serial.println(fileList[startIndex]);
                useFallback = false;
            }
        } else {
            Serial.println("start.anim not found — using fallback");
        }
    } else {
        Serial.println("SD not found — fallback only");
    }

    BTstack.setBLEDeviceConnectedCallback(bleConnected);
    BTstack.setBLEDeviceDisconnectedCallback(bleDisconnected);
    BTstack.setGATTCharacteristicWrite(bleWriteCallback);
    BTstack.setGATTCharacteristicNotificationCallback(bleNotifyCallback);
    BTstack.addGATTService(new UUID(NUS_SERVICE_UUID));
    rx_value_handle = BTstack.addGATTCharacteristicDynamic(
        new UUID("6E400002-B5A3-F393-E0A9-E50E24DCCA9E"),
        ATT_PROPERTY_WRITE | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE,
        NUS_RX_CHAR_ID);
    tx_value_handle = BTstack.addGATTCharacteristicDynamic(
        new UUID("6E400003-B5A3-F393-E0A9-E50E24DCCA9E"),
        ATT_PROPERTY_NOTIFY,
        NUS_TX_CHAR_ID);
    BTstack.setup("ProtoFace");
    BTstack.startAdvertising();
    Serial.println("BLE advertising as 'ProtoFace'");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    BTstack.loop();

    if (!frameLoaded) { delay(10); return; }

    uint8_t vol = readVolume();

    const LEDEntry *leds  = useFallback ? fallbackFrame.leds  : currentFrame.leds;
    uint8_t         tmode = useFallback ? fallbackFrame.timing_mode : currentFrame.timing_mode;
    uint16_t        dur   = useFallback ? fallbackFrame.duration_ms : currentFrame.duration_ms;

    pushFrame(leds, vol);

    if (!playing) { delayMicroseconds(16000); return; }

    if (tmode == TIMING_TIMED) {
        if (millis() - lastFrameTime >= dur) {
            lastFrameTime = millis();
            if (!useFallback) {
                currentFrameIdx++;
                if (currentFrameIdx >= totalFrames) {
                    currentFrameIdx = 0;
                    if (!rewindFile()) {
                        useFallback = true;
                    }
                } else {
                    if (!readNextFrame()) {
                        useFallback = true;
                    }
                }
            }
        }
    } else {
        delayMicroseconds(5000);
    }
}
