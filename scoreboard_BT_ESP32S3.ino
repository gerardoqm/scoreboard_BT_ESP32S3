#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Adafruit_GFX.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <ArduinoOTA.h> 
#include <Update.h>
#include <esp_random.h>
#include "mbedtls/md.h"

#define PANEL_RES_X 80
#define PANEL_RES_Y 40
#define PANEL_CHAIN_1 1

MatrixPanel_I2S_DMA *dma_display = nullptr;
Preferences prefs;

// Wi-Fi and OTA credentials live in secrets.h (gitignored). Copy secrets.h.example to create it.
#include "secrets.h"

// Safe Exposed ESP32-S3 HUB75 GPIO Pins
#define R1_PIN 1
#define G1_PIN 2
#define B1_PIN 3
#define R2_PIN 4
#define G2_PIN 5
#define B2_PIN 6
#define A_PIN  7
#define B_PIN  8
#define C_PIN  9
#define D_PIN  10
#define E_PIN  11  
#define LAT_PIN 12
#define OE_PIN  13
#define CLK_PIN 14

#define RGB_LED_PIN    48 
#define NUM_PIXELS      1
#define BATTERY_ADC_PIN 18 

Adafruit_NeoPixel statusLED(NUM_PIXELS, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

#define SERVICE_UUID        "12345678-1234-1234-1234-123456789012"
#define CHARACTERISTIC_UUID "87654321-4321-4321-4321-210987654321"
// CONFIG characteristic: [cmd, args...] writes, [cmd, status] notifications (status 0 = OK).
// Keep in sync with AdvancedBLEManager.swift.
#define CONFIG_UUID         "87654321-4321-4321-4321-210987654322"
#define CMD_SET_NAME 0x01   // args: 1-20 printable ASCII bytes; staged, not saved yet
#define CMD_APPLY    0x02   // saves the staged name and reboots
#define CMD_SET_COLORS 0x03 // args: 9 x RGB565 big-endian, in the PanelColor order below; saved to flash
#define CMD_ALERT      0x04 // args: type (1 = game start, 2 = period start) + 1-16 ASCII characters to scroll
#define CMD_SET_VISIBILITY 0x05 // args: 2 bytes, little-endian bitmask, bit i set = PanelColor i is shown; saved to flash
// Firmware update over Bluetooth: own characteristic. Phone -> board writes start with an opcode; replies are
// notifications [opcode, status, ...]. Data chunks are written without response and acknowledged every 4 KB.
#define OTA_UUID           "87654321-4321-4321-4321-210987654323"
#define OTA_OP_BEGIN       0x01   // -> notify [01, status, 16-byte random nonce]
#define OTA_OP_AUTH        0x02   // [02] + HMAC-SHA256(OTA_PASSWORD, nonce) (32 bytes) -> notify [02, status]
#define OTA_OP_START       0x03   // [03] + size (u32 LE) + CRC-32 (u32 LE) -> notify [03, status] once flash is ready
#define OTA_OP_DATA        0x04   // [04] + up to 180 bytes of the image, no response
#define OTA_OP_ACK         0x05   // notify only: [05] + bytes written to flash (u32 LE)
#define OTA_OP_END         0x06   // [06] -> verifies the CRC, switches to the new image, reboots; notify [06, status] first
#define OTA_OP_ABORT       0x07
#define OTA_OK             0x00
#define OTA_ERR_ARG        0x01
#define OTA_ERR_STATE      0x02
#define OTA_ERR_AUTH       0x03
#define OTA_ERR_SIZE       0x04
#define OTA_ERR_WRITE      0x05
#define OTA_ERR_CRC        0x06
#define OTA_ERR_LOCKED     0x07
#define OTA_ERR_TIMEOUT    0x08
#define OTA_RING_SIZE      16384
#define OTA_ACK_EVERY      4096
#define OTA_IDLE_TIMEOUT_MS 20000
#define ALERT_TEXT_MAX 16
#define ALERT_MS_PER_PX 18   // scroll speed of the alert text (about 55 px/s)
#define ALERT_FLASH_MS 250   // the border flashes at 2 Hz
#define ALERT_TAIL_MS 400    // flashing continues briefly after the text has left
#define CFG_OK       0x00
#define CFG_BAD_ARG  0x01
#define CFG_NOTHING_STAGED 0x02
#define DEVICE_NAME_MAX 20

// Protocol version reported in the telemetry notification. Bump when the packet layout changes.
// Telemetry (5 bytes in v1): [min, sec, running, period, battery%]
// v2 adds:                    [flags, protocolVersion]  (7 bytes)
// v3 adds the CMD_SET_COLORS and CMD_ALERT config commands (panel text colors, start-of-game/period alerts).
// v4 adds CMD_SET_VISIBILITY (hide or show each element of the panel).
// v5 adds firmware updates over Bluetooth (OTA characteristic) and the firmware version as bytes 7-9 of the telemetry (10 bytes).
#define PROTOCOL_VERSION 5
// Firmware version, semantic: bump PATCH for fixes, MINOR for features that stay compatible, MAJOR for breaking changes.
// Sent in the telemetry (bytes 7-9) so the app can show it and compare it with the image bundled in the app.
// PROTOCOL_VERSION above is only for the app's feature checks; the version below is what people see.
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 0
#define FW_VERSION_PATCH 1
#define FLAG_LOW_BATTERY    0x01  // battery below 15%
#define FLAG_RENAME_STAGED  0x02  // a name is staged and waiting for APPLY

// Panel layout (80x40), 3x5 pixel font: timer (left) and period (right) at 2x on top, scores at 3x in the
// middle, team names at 2x along the bottom. No frame.
#define NAME_MAX 20               // longest team name kept; names wider than the slot scroll
#define NAME_SLOT_W 36            // pixels available for a name: half the panel, minus margins
#define NAME_SLOT_H 10            // names are drawn at 2x the 3x5 font: 6 x 10 pixels per letter
#define MARQUEE_STEP_MS 70        // scroll speed: about 14 px/s, right to left
#define MARQUEE_GAP_PX 18         // blank space between the end of the name and its repeat
#define MARQUEE_DWELL_MS 1500     // pause at the start so the name can be read

// Text colors, in the order the app sends them (keep in sync with PanelElement in PanelColors.swift).
enum PanelColor { COLOR_TIMER_RUNNING, COLOR_TIMER_STOPPED, COLOR_PERIOD, COLOR_HOME_NAME, COLOR_HOME_SCORE,
                  COLOR_AWAY_NAME, COLOR_AWAY_SCORE, COLOR_ALERT_GAME, COLOR_ALERT_PERIOD, COLOR_COUNT };
uint16_t panelColors[COLOR_COUNT];
uint16_t panelVisible = 0x01FF;   // bit i set = element i is shown (all nine by default)


// Core Board Counters
int homeScore = 0, awayScore = 0, period = 1, totalPeriods = 4, brightness = 40;
int timerMin = 10, timerSec = 0, batteryPct = 100;
bool isTimerRunning = false;
bool hasReceivedState = false;  // false until the phone sends its first state packet this session

enum BLEState { SEARCHING_NEW, RETRYING_PAST, LINKED_CONNECTED };
BLEState currentBLEState = SEARCHING_NEW;

unsigned long stateTransitionMillis = 0;
unsigned long lastBlinkMillis = 0;
bool blinkToggleState = false;

String homeName = "HOME";
String awayName = "AWAY";
String deviceBroadcastName = "Scoreboard_Beta";

BLEServer *pServer = nullptr;
BLECharacteristic *pChar = nullptr;
BLECharacteristic *pConfigChar = nullptr;
String pendingDeviceName = "";

// The BLE callbacks run on another task than loop(), so state and drawing share one mutex.
SemaphoreHandle_t stateMutex = nullptr;
// Start-of-game / start-of-period alert: text scrolls across the whole panel while the border flashes.
bool alertActive = false;
unsigned long alertStartMillis = 0, lastAlertDraw = 0;
String alertText = "";
uint16_t alertColor = 0;
volatile bool sceneDirty = true;     // something changed and the panel needs a redraw
unsigned long lastTickMillis = 0;    // phase of the board's own 1 s clock tick
unsigned long lastMarqueeStep = 0;

enum FwuState { FWU_IDLE, FWU_AUTH_PENDING, FWU_READY, FWU_STARTING, FWU_RECEIVING };
volatile FwuState otaState = FWU_IDLE;
BLECharacteristic *pOtaChar = nullptr;
uint8_t otaNonce[16];
uint32_t otaSize = 0, otaExpectedCrc = 0, otaCrc = 0xFFFFFFFF, otaWritten = 0, otaLastAck = 0;
uint8_t otaRing[OTA_RING_SIZE];
volatile uint32_t otaHead = 0, otaTail = 0;     // running byte counters; the BLE task fills, loop() drains
volatile bool otaEndRequested = false, otaAbortRequested = false, otaOverflow = false;
uint8_t otaAuthFailures = 0;
unsigned long otaLockedUntil = 0, otaLastActivity = 0, lastOtaDraw = 0;

GFXcanvas16 *frame = nullptr;        // off-screen copy of the whole panel, copied over in one go (no flicker)
GFXcanvas16 *slotCanvas = nullptr;   // clips a scrolling name to its slot

// A team name that scrolls right to left when it is wider than its slot.
struct Marquee {
  String text;
  int offset = 0;
  unsigned long dwellUntil = 0;
};
Marquee homeMarquee, awayMarquee;

// 3x5 pixel font: digits, A-Z, "/", ":", space and "-". Each row is 3 bits (bit 2 = leftmost pixel).
struct Glyph { char ch; uint8_t w; uint8_t rows[5]; };
static const Glyph GLYPHS[] = {
  {'0', 3, {0x7,0x5,0x5,0x5,0x7}},
  {'1', 3, {0x2,0x6,0x2,0x2,0x7}},
  {'2', 3, {0x7,0x1,0x7,0x4,0x7}},
  {'3', 3, {0x7,0x1,0x7,0x1,0x7}},
  {'4', 3, {0x5,0x5,0x7,0x1,0x1}},
  {'5', 3, {0x7,0x4,0x7,0x1,0x7}},
  {'6', 3, {0x7,0x4,0x7,0x5,0x7}},
  {'7', 3, {0x7,0x1,0x2,0x2,0x2}},
  {'8', 3, {0x7,0x5,0x7,0x5,0x7}},
  {'9', 3, {0x7,0x5,0x7,0x1,0x7}},
  {'A', 3, {0x2,0x5,0x7,0x5,0x5}},
  {'B', 3, {0x6,0x5,0x6,0x5,0x6}},
  {'C', 3, {0x3,0x4,0x4,0x4,0x3}},
  {'D', 3, {0x6,0x5,0x5,0x5,0x6}},
  {'E', 3, {0x7,0x4,0x6,0x4,0x7}},
  {'F', 3, {0x7,0x4,0x6,0x4,0x4}},
  {'G', 3, {0x3,0x4,0x5,0x5,0x3}},
  {'H', 3, {0x5,0x5,0x7,0x5,0x5}},
  {'I', 3, {0x7,0x2,0x2,0x2,0x7}},
  {'J', 3, {0x1,0x1,0x1,0x5,0x2}},
  {'K', 3, {0x5,0x5,0x6,0x5,0x5}},
  {'L', 3, {0x4,0x4,0x4,0x4,0x7}},
  {'M', 3, {0x5,0x7,0x7,0x5,0x5}},
  {'N', 3, {0x7,0x5,0x5,0x5,0x5}},
  {'O', 3, {0x2,0x5,0x5,0x5,0x2}},
  {'P', 3, {0x6,0x5,0x6,0x4,0x4}},
  {'Q', 3, {0x2,0x5,0x5,0x6,0x3}},
  {'R', 3, {0x6,0x5,0x6,0x5,0x5}},
  {'S', 3, {0x3,0x4,0x2,0x1,0x6}},
  {'T', 3, {0x7,0x2,0x2,0x2,0x2}},
  {'U', 3, {0x5,0x5,0x5,0x5,0x7}},
  {'V', 3, {0x5,0x5,0x5,0x5,0x2}},
  {'W', 3, {0x5,0x5,0x7,0x7,0x5}},
  {'X', 3, {0x5,0x5,0x2,0x5,0x5}},
  {'Y', 3, {0x5,0x5,0x2,0x2,0x2}},
  {'Z', 3, {0x7,0x1,0x2,0x4,0x7}},
  {'/', 3, {0x1,0x1,0x2,0x4,0x4}},
  {'%', 3, {0x5,0x1,0x2,0x4,0x5}},
  {':', 1, {0x0,0x1,0x0,0x1,0x0}},
  {' ', 2, {0x0,0x0,0x0,0x0,0x0}},
  {'-', 3, {0x0,0x0,0x7,0x0,0x0}},
};
static const int GLYPH_COUNT = sizeof(GLYPHS) / sizeof(GLYPHS[0]);

// The timer has one switch for both of its colors: running and stopped share the running bit.
static inline bool shown(PanelColor c) {
  if (c == COLOR_TIMER_STOPPED) c = COLOR_TIMER_RUNNING;
  return (panelVisible >> c) & 1;
}

static const Glyph *glyphFor(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;               // the font has capitals only
  for (int i = 0; i < GLYPH_COUNT; i++) if (GLYPHS[i].ch == c) return &GLYPHS[i];
  return glyphFor('-');                            // anything else (accents are stripped by the app anyway)
}

// Width of a string in font pixels (1 pixel between letters).
static int textUnits(const String &t) {
  if (t.length() == 0) return 0;
  int u = -1;
  for (size_t i = 0; i < t.length(); i++) u += glyphFor(t[i])->w + 1;
  return u;
}

// Draws text at pixel scale s. Drawing outside the canvas is clipped.
static void drawText(GFXcanvas16 *cv, int x, int y, const String &t, uint16_t color, int s) {
  for (size_t i = 0; i < t.length(); i++) {
    const Glyph *g = glyphFor(t[i]);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < g->w; col++)
        if (g->rows[row] & (1 << (g->w - 1 - col)))
          for (int dx = 0; dx < s; dx++) for (int dy = 0; dy < s; dy++) cv->drawPixel(x + col * s + dx, y + row * s + dy, color);
    x += (g->w + 1) * s;
  }
}


static inline uint16_t c565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

void marqueeSet(Marquee &m, const String &text) {
  if (m.text != text) { m.text = text; m.offset = 0; m.dwellUntil = millis() + MARQUEE_DWELL_MS; }
}

// Advances the scroll by one pixel. Returns true when the picture changed.
bool marqueeStep(Marquee &m) {
  int w = textUnits(m.text) * 2;
  if (w <= NAME_SLOT_W) { m.offset = 0; return false; }
  if (millis() < m.dwellUntil) return false;
  m.offset++;
  if (m.offset >= w + MARQUEE_GAP_PX) { m.offset = 0; m.dwellUntil = millis() + MARQUEE_DWELL_MS; }
  return true;
}

int readBatteryPercentage() {
  long rawSum = 0;
  for(int i = 0; i < 10; i++) { rawSum += analogRead(BATTERY_ADC_PIN); delay(2); }
  float analogVolts = (rawSum / 10.0) * (3.3 / 4095.0);
  float calculatedBatteryVolts = analogVolts * 2.0; 
  int pct = (int)((calculatedBatteryVolts - 3.4) / (4.2 - 3.4) * 100.0);
  return max(0, min(pct, 100));
}

// Defaults match the colors the panel used before they were configurable; saved colors replace them.
void loadPanelColors() {
  panelColors[COLOR_TIMER_RUNNING] = c565(0, 255, 0);
  panelColors[COLOR_TIMER_STOPPED] = c565(255, 120, 0);
  panelColors[COLOR_PERIOD]        = c565(255, 255, 0);
  panelColors[COLOR_HOME_NAME]     = c565(0, 120, 255);
  panelColors[COLOR_HOME_SCORE]    = c565(0, 120, 255);
  panelColors[COLOR_AWAY_NAME]     = c565(255, 40, 40);
  panelColors[COLOR_AWAY_SCORE]    = c565(255, 40, 40);
  panelColors[COLOR_ALERT_GAME]    = c565(0, 255, 0);
  panelColors[COLOR_ALERT_PERIOD]  = c565(0, 220, 255);
  prefs.begin("scoreboard", true);
  if (prefs.getBytesLength("colors") == sizeof(panelColors)) prefs.getBytes("colors", panelColors, sizeof(panelColors));
  panelVisible = prefs.getUShort("visible", 0x01FF) & 0x01FF;
  prefs.end();
}

// One frame of the alert: flashing border plus the text scrolling right to left at 3x. Call with stateMutex held.
void drawAlertFrame(unsigned long elapsed) {
  frame->fillScreen(0);
  if ((elapsed / ALERT_FLASH_MS) % 2 == 0) {
    frame->drawRect(0, 0, PANEL_RES_X, PANEL_RES_Y, alertColor);
    frame->drawRect(1, 1, PANEL_RES_X - 2, PANEL_RES_Y - 2, alertColor);
  }
  drawText(frame, PANEL_RES_X - (int)(elapsed / ALERT_MS_PER_PX), 12, alertText, alertColor, 3);
  dma_display->drawRGBBitmap(0, 0, frame->getBuffer(), PANEL_RES_X, PANEL_RES_Y);
}

void drawName(Marquee &m, int slotX, uint16_t color) {
  slotCanvas->fillScreen(0);
  int w = textUnits(m.text) * 2;
  if (w <= NAME_SLOT_W) {                       // fits: centered in the slot
    drawText(slotCanvas, (NAME_SLOT_W - w) / 2, 0, m.text, color, 2);
  } else {                                      // too wide: scroll, with a second copy trailing for a seamless loop
    drawText(slotCanvas, -m.offset, 0, m.text, color, 2);
    drawText(slotCanvas, -m.offset + w + MARQUEE_GAP_PX, 0, m.text, color, 2);
  }
  frame->drawRGBBitmap(slotX, 29, slotCanvas->getBuffer(), NAME_SLOT_W, NAME_SLOT_H);
}

void drawScore(int value, int centerX, uint16_t color) {
  String text = String(value);
  drawText(frame, centerX - textUnits(text) * 3 / 2, 13, text, color, 3);
}

// Draws the whole scoreboard off-screen, then copies it to the panel. Call with stateMutex held.
void drawScoreboard() {
  frame->fillScreen(0);

  // top strip, from the header bytes (always delivered, unlike the optional text):
  // timer on the left (green while running, orange when stopped), period on the right
  char clock[6];
  snprintf(clock, sizeof(clock), "%02d:%02d", timerMin, timerSec);
  if (shown(COLOR_TIMER_RUNNING)) drawText(frame, 2, 1, String(clock), panelColors[isTimerRunning ? COLOR_TIMER_RUNNING : COLOR_TIMER_STOPPED], 2);
  String periodText = String(period) + "/" + String(totalPeriods);
  if (shown(COLOR_PERIOD)) drawText(frame, 78 - textUnits(periodText) * 2, 1, periodText, panelColors[COLOR_PERIOD], 2);

  frame->drawFastVLine(40, 12, 26, c565(40, 40, 40));   // between the two teams

  if (shown(COLOR_HOME_SCORE)) drawScore(homeScore, 20, panelColors[COLOR_HOME_SCORE]);
  if (shown(COLOR_AWAY_SCORE)) drawScore(awayScore, 60, panelColors[COLOR_AWAY_SCORE]);
  if (shown(COLOR_HOME_NAME)) drawName(homeMarquee, 2, panelColors[COLOR_HOME_NAME]);
  if (shown(COLOR_AWAY_NAME)) drawName(awayMarquee, 42, panelColors[COLOR_AWAY_NAME]);

  dma_display->drawRGBBitmap(0, 0, frame->getBuffer(), PANEL_RES_X, PANEL_RES_Y);
}

// Progress screen shown while a firmware update runs. Call with stateMutex held.
void drawOtaScreen() {
  frame->fillScreen(0);
  String title = "UPDATING";
  drawText(frame, (PANEL_RES_X - textUnits(title) * 2) / 2, 4, title, c565(255, 200, 0), 2);
  uint32_t percent = otaSize ? (uint32_t)((uint64_t)otaWritten * 100 / otaSize) : 0;
  frame->drawRect(5, 20, 70, 10, c565(80, 80, 80));
  frame->fillRect(6, 21, (int)(68 * percent / 100), 8, c565(0, 255, 0));
  String label = String(percent) + "%";
  drawText(frame, (PANEL_RES_X - textUnits(label)) / 2, 33, label, c565(255, 255, 255), 1);
  dma_display->drawRGBBitmap(0, 0, frame->getBuffer(), PANEL_RES_X, PANEL_RES_Y);
}

class ScoreboardServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { currentBLEState = LINKED_CONNECTED; statusLED.setPixelColor(0, statusLED.Color(0, 150, 0)); statusLED.show(); }
    void onDisconnect(BLEServer* pServer) {
        if (otaState != FWU_IDLE) otaAbortRequested = true;     // the phone vanished mid-update
        if (!hasReceivedState) { currentBLEState = SEARCHING_NEW; } 
        else { currentBLEState = RETRYING_PAST; stateTransitionMillis = millis(); }
        statusLED.setPixelColor(0, statusLED.Color(0, 0, 0)); statusLED.show();
        delay(200); pServer->startAdvertising();
    }
};

class FullControlCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        uint8_t* rawData = pCharacteristic->getData();
        size_t dataLength = pCharacteristic->getLength();
        
        if (dataLength >= 8) {
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            hasReceivedState = true;
            homeScore      = rawData[0];
            awayScore      = rawData[1];
            period         = rawData[2];
            brightness     = constrain(rawData[3], 10, 120); // same range as the app's stepper
            timerMin       = rawData[4];
            timerSec       = rawData[5];
            isTimerRunning = (rawData[6] == 1);
            totalPeriods   = rawData[7];
            lastTickMillis = millis();   // line the board's own seconds up with the phone's

            dma_display->setBrightness8(brightness);

            // Optional text: "home|away|ratio|label". Only the names are used; the period box and the
            // clock are drawn from the header bytes above, which fit in any BLE packet size.
            if (dataLength > 8) {
                String payload = "";
                for (size_t i = 8; i < dataLength; i++) { payload += (char)rawData[i]; }
                int pipe1 = payload.indexOf('|');
                if (pipe1 != -1) {
                    int pipe2 = payload.indexOf('|', pipe1 + 1);
                    homeName = payload.substring(0, pipe1).substring(0, NAME_MAX);
                    awayName = (pipe2 != -1 ? payload.substring(pipe1 + 1, pipe2) : payload.substring(pipe1 + 1)).substring(0, NAME_MAX);
                    homeName.toUpperCase(); awayName.toUpperCase();
                }
            }
            marqueeSet(homeMarquee, homeName);
            marqueeSet(awayMarquee, awayName);
            sceneDirty = true;
            xSemaphoreGive(stateMutex);
        }
    }
};

class ConfigCallbacks: public BLECharacteristicCallbacks {
    void reply(uint8_t cmd, uint8_t status) {
        uint8_t resp[2] = { cmd, status };
        pConfigChar->setValue(resp, 2);
        pConfigChar->notify();
    }

    void onWrite(BLECharacteristic *pCharacteristic) {
        uint8_t* data = pCharacteristic->getData();
        size_t len = pCharacteristic->getLength();
        if (len < 1) return;

        switch (data[0]) {
            case CMD_SET_NAME: {
                size_t nameLen = len - 1;
                bool valid = nameLen >= 1 && nameLen <= DEVICE_NAME_MAX;
                for (size_t i = 1; valid && i < len; i++) { valid = (data[i] >= 0x20 && data[i] <= 0x7E); }
                if (!valid) { reply(CMD_SET_NAME, CFG_BAD_ARG); return; }
                pendingDeviceName = "";
                for (size_t i = 1; i < len; i++) { pendingDeviceName += (char)data[i]; }
                reply(CMD_SET_NAME, CFG_OK);
                break;
            }
            case CMD_SET_COLORS: {
                if (len != 1 + COLOR_COUNT * 2) { reply(CMD_SET_COLORS, CFG_BAD_ARG); return; }
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                for (int i = 0; i < COLOR_COUNT; i++) panelColors[i] = ((uint16_t)data[1 + i * 2] << 8) | data[2 + i * 2];
                sceneDirty = true;
                xSemaphoreGive(stateMutex);
                prefs.begin("scoreboard", false); prefs.putBytes("colors", panelColors, sizeof(panelColors)); prefs.end();
                reply(CMD_SET_COLORS, CFG_OK);
                break;
            }
            case CMD_SET_VISIBILITY: {
                if (len != 3) { reply(CMD_SET_VISIBILITY, CFG_BAD_ARG); return; }
                uint16_t mask = ((uint16_t)data[1] | ((uint16_t)data[2] << 8)) & 0x01FF;
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                panelVisible = mask;
                sceneDirty = true;
                xSemaphoreGive(stateMutex);
                prefs.begin("scoreboard", false); prefs.putUShort("visible", mask); prefs.end();
                reply(CMD_SET_VISIBILITY, CFG_OK);
                break;
            }
            case CMD_ALERT: {
                size_t textLen = len - 2;
                bool valid = len >= 3 && textLen <= ALERT_TEXT_MAX && (data[1] == 1 || data[1] == 2);
                for (size_t i = 2; valid && i < len; i++) { valid = (data[i] >= 0x20 && data[i] <= 0x7E); }
                if (!valid) { reply(CMD_ALERT, CFG_BAD_ARG); return; }
                if (!shown(data[1] == 1 ? COLOR_ALERT_GAME : COLOR_ALERT_PERIOD)) { reply(CMD_ALERT, CFG_OK); return; }   // this alert is switched off
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                alertText = "";
                for (size_t i = 2; i < len; i++) { alertText += (char)data[i]; }
                alertText.toUpperCase();
                alertColor = panelColors[data[1] == 1 ? COLOR_ALERT_GAME : COLOR_ALERT_PERIOD];
                alertStartMillis = millis();
                alertActive = true;
                xSemaphoreGive(stateMutex);
                reply(CMD_ALERT, CFG_OK);
                break;
            }
            case CMD_APPLY: {
                if (pendingDeviceName.length() == 0) { reply(CMD_APPLY, CFG_NOTHING_STAGED); return; }
                deviceBroadcastName = pendingDeviceName;
                pendingDeviceName = "";
                prefs.begin("scoreboard", false); prefs.putString("devName", deviceBroadcastName); prefs.end();
                reply(CMD_APPLY, CFG_OK);
                delay(500); esp_restart();
                break;
            }
            default:
                reply(data[0], CFG_BAD_ARG);
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Firmware update over Bluetooth. The phone proves it knows OTA_PASSWORD (HMAC over a random nonce), then
// streams the image; chunks wait in a ring buffer so the BLE task never blocks on flash. loop() writes them
// to the spare firmware slot, checks the CRC-32 and only then switches to the new image.
// ---------------------------------------------------------------------------------------------

// Standard CRC-32 (the same one zlib uses); start from 0xFFFFFFFF and invert at the end.
static uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

static void otaNotifyData(uint8_t op, uint8_t status, const uint8_t *extra, size_t extraLen) {
  uint8_t buf[20];
  buf[0] = op; buf[1] = status;
  if (extra == nullptr || extraLen > sizeof(buf) - 2) extraLen = 0; else memcpy(buf + 2, extra, extraLen);
  pOtaChar->setValue(buf, 2 + extraLen);
  pOtaChar->notify();
}

// Gives up on the current update: discards what was written, tells the phone why, and returns to the scoreboard.
static void otaFail(uint8_t op, uint8_t status) {
  if (otaState == FWU_STARTING || otaState == FWU_RECEIVING) Update.abort();
  otaState = FWU_IDLE;
  otaEndRequested = false; otaAbortRequested = false; otaOverflow = false;
  otaHead = otaTail = 0;
  otaNotifyData(op, status, nullptr, 0);
  sceneDirty = true;
}

class OtaCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        uint8_t *d = pCharacteristic->getData();
        size_t len = pCharacteristic->getLength();
        if (len < 1) return;
        otaLastActivity = millis();

        switch (d[0]) {
            case OTA_OP_BEGIN: {
                if (otaState == FWU_STARTING || otaState == FWU_RECEIVING) { otaNotifyData(OTA_OP_BEGIN, OTA_ERR_STATE, nullptr, 0); return; }
                if (millis() < otaLockedUntil) { otaNotifyData(OTA_OP_BEGIN, OTA_ERR_LOCKED, nullptr, 0); return; }
                esp_fill_random(otaNonce, sizeof(otaNonce));
                otaState = FWU_AUTH_PENDING;
                otaNotifyData(OTA_OP_BEGIN, OTA_OK, otaNonce, sizeof(otaNonce));
                break;
            }
            case OTA_OP_AUTH: {
                if (otaState != FWU_AUTH_PENDING || len != 33) { otaNotifyData(OTA_OP_AUTH, OTA_ERR_STATE, nullptr, 0); return; }
                uint8_t mac[32];
                const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
                mbedtls_md_hmac(info, (const unsigned char *)OTA_PASSWORD, strlen(OTA_PASSWORD), otaNonce, sizeof(otaNonce), mac);
                uint8_t diff = 0;
                for (int i = 0; i < 32; i++) diff |= mac[i] ^ d[1 + i];     // constant time
                if (diff == 0) {
                    otaState = FWU_READY; otaAuthFailures = 0;
                    otaNotifyData(OTA_OP_AUTH, OTA_OK, nullptr, 0);
                } else {
                    otaState = FWU_IDLE;
                    if (++otaAuthFailures >= 3) { otaLockedUntil = millis() + 30000; otaAuthFailures = 0; }   // 3 wrong tries: locked for 30 s
                    otaNotifyData(OTA_OP_AUTH, OTA_ERR_AUTH, nullptr, 0);
                }
                break;
            }
            case OTA_OP_START: {
                if (otaState != FWU_READY || len != 9) { otaNotifyData(OTA_OP_START, OTA_ERR_STATE, nullptr, 0); return; }
                uint32_t size = (uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
                uint32_t crc  = (uint32_t)d[5] | ((uint32_t)d[6] << 8) | ((uint32_t)d[7] << 16) | ((uint32_t)d[8] << 24);
                if (size < 65536 || size > ESP.getFreeSketchSpace()) { otaNotifyData(OTA_OP_START, OTA_ERR_SIZE, nullptr, 0); return; }
                otaSize = size; otaExpectedCrc = crc; otaCrc = 0xFFFFFFFF; otaWritten = 0; otaLastAck = 0;
                otaHead = otaTail = 0; otaEndRequested = false; otaAbortRequested = false; otaOverflow = false;
                otaState = FWU_STARTING;       // loop() erases the slot (slow) and then confirms with a START notification
                break;
            }
            case OTA_OP_DATA: {
                if (otaState != FWU_RECEIVING) return;
                size_t n = len - 1;
                uint32_t used = otaHead - otaTail;
                if (n > OTA_RING_SIZE - used) { otaOverflow = true; return; }
                for (size_t i = 0; i < n; i++) otaRing[(otaHead + i) % OTA_RING_SIZE] = d[1 + i];
                __sync_synchronize();
                otaHead = otaHead + n;
                break;
            }
            case OTA_OP_END:
                if (otaState == FWU_RECEIVING) otaEndRequested = true; else otaNotifyData(OTA_OP_END, OTA_ERR_STATE, nullptr, 0);
                break;
            case OTA_OP_ABORT:
                otaAbortRequested = true;
                break;
        }
    }
};

// Called from loop(): erases and writes flash, acknowledges progress, verifies and applies the finished image.
static void otaService() {
  if (otaState == FWU_IDLE) return;
  unsigned long now = millis();

  if (otaAbortRequested) {
    if (otaState == FWU_STARTING || otaState == FWU_RECEIVING) Update.abort();
    otaState = FWU_IDLE; otaAbortRequested = false; otaEndRequested = false; otaHead = otaTail = 0;
    sceneDirty = true;
    return;
  }
  if (now - otaLastActivity > OTA_IDLE_TIMEOUT_MS) { otaFail(OTA_OP_ABORT, OTA_ERR_TIMEOUT); return; }

  if (otaState == FWU_STARTING) {
    if (Update.begin(otaSize)) {
      otaState = FWU_RECEIVING; otaLastActivity = millis();
      otaNotifyData(OTA_OP_START, OTA_OK, nullptr, 0);
    } else {
      otaState = FWU_IDLE; sceneDirty = true;
      otaNotifyData(OTA_OP_START, OTA_ERR_WRITE, nullptr, 0);
    }
    return;
  }

  if (otaState == FWU_RECEIVING) {
    if (otaOverflow) { otaFail(OTA_OP_DATA, OTA_ERR_WRITE); return; }
    uint32_t avail = otaHead - otaTail;
    while (avail > 0) {
      uint32_t idx = otaTail % OTA_RING_SIZE;
      uint32_t n = min(avail, (uint32_t)(OTA_RING_SIZE - idx));
      if (n > 2048) n = 2048;
      if (Update.write(&otaRing[idx], n) != n) { otaFail(OTA_OP_DATA, OTA_ERR_WRITE); return; }
      otaCrc = crc32Update(otaCrc, &otaRing[idx], n);
      otaWritten += n;
      __sync_synchronize();
      otaTail = otaTail + n;
      avail -= n;
      otaLastActivity = millis();
    }
    if (otaWritten - otaLastAck >= OTA_ACK_EVERY || (otaWritten == otaSize && otaLastAck != otaWritten)) {
      otaLastAck = otaWritten;
      uint8_t ack[5] = { OTA_OP_ACK, (uint8_t)otaWritten, (uint8_t)(otaWritten >> 8), (uint8_t)(otaWritten >> 16), (uint8_t)(otaWritten >> 24) };
      pOtaChar->setValue(ack, sizeof(ack));
      pOtaChar->notify();
    }
    if (otaEndRequested && otaHead == otaTail) {
      if (otaWritten != otaSize)              otaFail(OTA_OP_END, OTA_ERR_SIZE);
      else if ((~otaCrc) != otaExpectedCrc)   otaFail(OTA_OP_END, OTA_ERR_CRC);
      else if (!Update.end(true))             otaFail(OTA_OP_END, OTA_ERR_WRITE);
      else {                                   // image is in place and verified: tell the phone, then boot into it
        otaNotifyData(OTA_OP_END, OTA_OK, nullptr, 0);
        delay(800);
        ESP.restart();
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  analogSetAttenuation(ADC_11db); 
  statusLED.begin(); statusLED.setBrightness(30); statusLED.show();

  prefs.begin("scoreboard", false);
  deviceBroadcastName = prefs.getString("devName", "Scoreboard_Beta");
  prefs.end();

  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN_1);
  mxconfig.gpio.r1 = R1_PIN; mxconfig.gpio.g1 = G1_PIN; mxconfig.gpio.b1 = B1_PIN;
  mxconfig.gpio.r2 = R2_PIN; mxconfig.gpio.g2 = G2_PIN; mxconfig.gpio.b2 = B2_PIN;
  mxconfig.gpio.a  = A_PIN;  mxconfig.gpio.b  = B_PIN;  mxconfig.gpio.c  = C_PIN; mxconfig.gpio.d = D_PIN; 
  mxconfig.gpio.e  = E_PIN;
  mxconfig.gpio.lat = LAT_PIN; mxconfig.gpio.oe = OE_PIN; mxconfig.gpio.clk = CLK_PIN;
  mxconfig.clkphase = false; mxconfig.driver = HUB75_I2S_CFG::SHIFTREG;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig); dma_display->begin();
  dma_display->setBrightness8(brightness);
  loadPanelColors();
  frame = new GFXcanvas16(PANEL_RES_X, PANEL_RES_Y);
  slotCanvas = new GFXcanvas16(NAME_SLOT_W, NAME_SLOT_H);
  stateMutex = xSemaphoreCreateMutex();
  marqueeSet(homeMarquee, homeName);
  marqueeSet(awayMarquee, awayName);
  drawScoreboard();

  // 📡 SILENT BACKGROUND WI-FI OTA INITIALIZATION
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("[wifi] connected, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.printf("[wifi] disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
    }
  });
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[wifi] connecting...");
  
  // Set up local Arduino OTA network port name
  // Hostnames may only contain letters, digits and "-": "Scoreboard_Beta" becomes "Scoreboard-Beta".
  String otaHost = deviceBroadcastName;
  for (size_t i = 0; i < otaHost.length(); i++) {
    char c = otaHost[i];
    if (!isalnum((unsigned char)c) && c != '-') otaHost.setCharAt(i, '-');
  }
  char otaNameBuffer[32];
  otaHost.toCharArray(otaNameBuffer, 32);
  ArduinoOTA.setHostname(otaNameBuffer);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() { Serial.println("Wireless flashing started..."); });
  ArduinoOTA.onEnd([]() { Serial.println("\nWireless flash complete! Rebooting..."); });
  // ArduinoOTA.begin() (and its mDNS announcement) is called from loop() once Wi-Fi has an IP.

  // Bluetooth configuration setup
  char nameBuffer[32];
  deviceBroadcastName.toCharArray(nameBuffer, 32);
  BLEDevice::init(nameBuffer);
  BLEDevice::setMTU(185);   // default is 23 (20 bytes per write), too small for names plus the 8-byte header
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ScoreboardServerCallbacks()); 
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pChar = pService->createCharacteristic(
                        CHARACTERISTIC_UUID,
                        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
                      );

  pChar->setCallbacks(new FullControlCallbacks());

  pConfigChar = pService->createCharacteristic(
                        CONFIG_UUID,
                        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
                      );
  pConfigChar->setCallbacks(new ConfigCallbacks());

  pOtaChar = pService->createCharacteristic(
                        OTA_UUID,
                        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY
                      );
  pOtaChar->setCallbacks(new OtaCallbacks());
  pService->start();
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  
  currentBLEState = SEARCHING_NEW;
}

void loop() {
  unsigned long currentMillis = millis();

  otaService();   // firmware update over Bluetooth: flash writes happen here, outside the state mutex

  // Clock, scrolling names and redraw. The clock keeps counting here even if the phone link drops.
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (isTimerRunning && currentMillis - lastTickMillis >= 1000) {
    lastTickMillis += 1000;
    if (currentMillis - lastTickMillis > 2000) lastTickMillis = currentMillis;  // far behind: resync instead of catching up
    if (timerSec > 0) timerSec--;
    else if (timerMin > 0) { timerMin--; timerSec = 59; }
    else isTimerRunning = false;
    sceneDirty = true;
  }
  if (currentMillis - lastMarqueeStep >= MARQUEE_STEP_MS) {
    lastMarqueeStep = currentMillis;
    bool movedHome = marqueeStep(homeMarquee), movedAway = marqueeStep(awayMarquee);
    if (movedHome || movedAway) sceneDirty = true;
  }
  if (otaState != FWU_IDLE) {
    if (currentMillis - lastOtaDraw >= 150) { lastOtaDraw = currentMillis; drawOtaScreen(); }   // progress bar instead of the scoreboard
  } else if (alertActive) {
    unsigned long elapsed = currentMillis - alertStartMillis;
    unsigned long total = (unsigned long)(PANEL_RES_X + textUnits(alertText) * 3) * ALERT_MS_PER_PX + ALERT_TAIL_MS;
    if (elapsed >= total) { alertActive = false; sceneDirty = true; }          // back to the scoreboard
    else if (currentMillis - lastAlertDraw >= 20) { lastAlertDraw = currentMillis; drawAlertFrame(elapsed); }
  } else if (sceneDirty) { sceneDirty = false; drawScoreboard(); }
  xSemaphoreGive(stateMutex);

  // Handle background wireless Arduino IDE port listening check requests
  static unsigned long lastWifiLogMillis = 0;
  if (currentMillis - lastWifiLogMillis >= 10000) {
    lastWifiLogMillis = currentMillis;
    // status: 3 = connected, 1 = network not found (not visible on 2.4 GHz?), 4 = connect failed (wrong password?), 6 = disconnected
    Serial.printf("[wifi] status %d, IP %s, ble %s\n", WiFi.status(), WiFi.localIP().toString().c_str(),
                  currentBLEState == LINKED_CONNECTED ? "linked" : "waiting");
  }
  static bool otaStarted = false;
  if (!otaStarted && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.begin();
    otaStarted = true;
    Serial.printf("[ota] ready as %s.local\n", ArduinoOTA.getHostname().c_str());
  }
  if (otaStarted) ArduinoOTA.handle();

  static unsigned long lastNotifyMillis = 0;
  if (currentBLEState == LINKED_CONNECTED && currentMillis - lastNotifyMillis >= 2000) {
     lastNotifyMillis = currentMillis;
     batteryPct = readBatteryPercentage();
     
     // FIXED: Added array assignment tracking brackets to structure multi-byte list payloads
     uint8_t flags = (batteryPct < 15 ? FLAG_LOW_BATTERY : 0) | (pendingDeviceName.length() > 0 ? FLAG_RENAME_STAGED : 0);
     uint8_t telemetryPacket[] = { (uint8_t)timerMin, (uint8_t)timerSec, (uint8_t)(isTimerRunning ? 1 : 0), (uint8_t)period, (uint8_t)batteryPct, flags, PROTOCOL_VERSION, FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH };
     pChar->setValue(telemetryPacket, sizeof(telemetryPacket)); pChar->notify();
  }

  if (currentMillis - lastBlinkMillis >= 400) {
    lastBlinkMillis = currentMillis;
    blinkToggleState = !blinkToggleState;

    if (currentBLEState == SEARCHING_NEW) {
      statusLED.setPixelColor(0, blinkToggleState ? statusLED.Color(0, 0, 200) : statusLED.Color(0, 0, 0)); statusLED.show();
    } 
    else if (currentBLEState == RETRYING_PAST) {
      statusLED.setPixelColor(0, blinkToggleState ? statusLED.Color(0, 150, 200) : statusLED.Color(0, 0, 0)); statusLED.show();
      if (currentMillis - stateTransitionMillis >= 30000) {
        currentBLEState = SEARCHING_NEW;
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        hasReceivedState = false;
        homeScore = 0; awayScore = 0; period = 1; totalPeriods = 4; homeName = "HOME"; awayName = "AWAY";
        timerMin = 10; timerSec = 0; isTimerRunning = false;
        marqueeSet(homeMarquee, homeName); marqueeSet(awayMarquee, awayName);
        sceneDirty = true;
        xSemaphoreGive(stateMutex);
      }
    }
  }
  delay(10);
}
