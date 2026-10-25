#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Adafruit_GFX.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Update.h>
#include <esp_random.h>
#include "mbedtls/md.h"

#define PANEL_RES_X 80
#define PANEL_RES_Y 40
#define PANEL_CHAIN_1 1

MatrixPanel_I2S_DMA *dma_display = nullptr;
Preferences prefs;

// The Bluetooth update key (OTA_PASSWORD) lives in secrets.h (gitignored). Copy secrets.h.example to create it.
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
#define CMD_ALERT      0x04 // args: type (1 = game start, 2 = period start, 3 = goal, with the icon if one is loaded) + 1-32 ASCII characters to scroll
#define CMD_SET_VISIBILITY 0x05 // args: 2 bytes, little-endian bitmask, bit i set = PanelColor i is shown; saved to flash
#define CMD_ALERT_ICON 0x06 // args: first row (0-15) + 1-4 rows of 16 pixels, RGB565 big-endian (32 bytes per row); no args = remove the icon
                            // The goal alert (CMD_ALERT type 3) scrolls this 16x16 picture in front of its text.
#define CMD_NAME_ICON  0x07 // args: slot (0-3), first row (0-9) + 1-5 rows of 10 pixels, RGB565 big-endian (20 bytes per row); slot alone = clear it
                            // A team name shows icon N (1-4) where it holds the byte N: slots 0-1 for the home name, 2-3 for the away name.
#define CMD_END_TEXT   0x08 // args: 1-32 ASCII characters: what the board scrolls by itself when the last period runs out (the app sends it in its language)
#define CMD_MIRROR     0x09 // args: 1 = look for a second board (60 s), 2 = stop looking, 3 = unlink the second board. The second board mirrors this one over ESP-NOW.
                            // Link the second board by holding its BOOT button for 2 s while the first one is looking. Hold BOOT for 6 s to unlink from the second board.
#define CMD_PANEL      0x0A // args: preset 0-11 (see PANEL_PRESETS below): saves the panel driver preference and restarts the board
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
#define OTA_ACK_EVERY      2048
#define OTA_ACK_REPEAT_MS  250    // the progress ACK is repeated while receiving, so one lost notification cannot freeze the transfer
#define OTA_IDLE_TIMEOUT_MS 20000
#define ALERT_TEXT_MAX 32
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
// v6 adds CMD_ALERT_ICON and the goal alert (CMD_ALERT type 3).
// v7 adds CMD_NAME_ICON: emoji pictures inside team names.
// v8 raises the alert text limit from 16 to 32 characters (long team names in goal alerts).
// v9 adds CMD_END_TEXT and the board announces the end of the match by itself.
// v10 adds CMD_MIRROR: a second board shows the same scoreboard, linked board to board with ESP-NOW. Telemetry gets a byte 10 (second board's battery).
// v11 adds CMD_PANEL (choose the panel's driver chip setting) and telemetry byte 11 (the current preset).
// v5 adds firmware updates over Bluetooth (OTA characteristic) and the firmware version as bytes 7-9 of the telemetry (10 bytes).
#define PROTOCOL_VERSION 11
// Firmware version, semantic: bump PATCH for fixes, MINOR for features that stay compatible, MAJOR for breaking changes.
// Sent in the telemetry (bytes 7-9) so the app can show it and compare it with the image bundled in the app.
// PROTOCOL_VERSION above is only for the app's feature checks; the version below is what people see.
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 5
#define FW_VERSION_PATCH 1
#define FLAG_LOW_BATTERY    0x01  // battery below 15%
#define FLAG_RENAME_STAGED  0x02  // a name is staged and waiting for APPLY
#define FLAG_MIRROR_SOURCE  0x04  // this board feeds a second board
#define FLAG_MIRROR_ONLINE  0x08  // ... and that board answered in the last few seconds
#define FLAG_MIRROR_FOLLOWER 0x10 // this board follows another board instead of the phone
#define FLAG_MIRROR_PAIRING 0x20  // looking for a second board

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

// Panel driver presets. HUB75 is one-way (the panel cannot be asked what chip it has), so the right setting is found by eye:
// the diagnose mode (BOOT within 2 s after power-up) and the app's Panel type page step through these and save the one that looks right.
// Preset 0 is the default and what the firmware always used. Keep the names in sync with PanelType in the app.
#define PANEL_PRESETS 12
static const char *PANEL_PRESET_NAMES[PANEL_PRESETS] = { "SHIFTREG", "SHIFTREG B", "FM6124", "FM6124 B", "FM6126A", "FM6126A B",
                                                         "ICN2038S", "ICN2038S B", "MBI5124", "MBI5124 B", "DP3246", "DP3246 B" };
static uint8_t panelPreset = 0;    // driver = preset / 2, clock phase inverted for the "B" presets (odd numbers)


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
String matchEndText = "FULL TIME";     // set by the app (CMD_END_TEXT); the board announces the end of the match by itself, even if the phone is gone
#define NAME_ICON_PX 10                 // emoji inside team names: 10 x 10 pixels, the height of a name line
uint16_t nameIcons[4][NAME_ICON_PX * NAME_ICON_PX];   // RGB565, 0 = transparent
uint16_t alertIcon[16 * 16];            // goal icon, RGB565; 0 = transparent
uint16_t alertIconRows = 0;             // bit r set = row r has been received; all 16 = the icon is complete
bool alertUsesIcon = false;
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

// The bytes 1-4 in a name stand for the emoji pictures in nameIcons; they are 5 font units (10 pixels at 2x) wide.
static inline bool isIconChar(char c) { return c >= 1 && c <= 4; }

// Width of a string in font pixels (1 pixel between letters).
static int textUnits(const String &t) {
  if (t.length() == 0) return 0;
  int u = -1;
  for (size_t i = 0; i < t.length(); i++) u += (isIconChar(t[i]) ? 5 : glyphFor(t[i])->w) + 1;
  return u;
}

// Draws text at pixel scale s. Drawing outside the canvas is clipped.
static void drawText(GFXcanvas16 *cv, int x, int y, const String &t, uint16_t color, int s) {
  for (size_t i = 0; i < t.length(); i++) {
    if (isIconChar(t[i])) {                        // emoji picture: 10 pixels at the 2x name size, 15 at the 3x alert size
      if (s == 2 || s == 3) {
        const uint16_t *icon = nameIcons[t[i] - 1];
        int n = NAME_ICON_PX * s / 2;
        for (int r = 0; r < n; r++) for (int c = 0; c < n; c++) {
          uint16_t px = icon[(r * NAME_ICON_PX / n) * NAME_ICON_PX + (c * NAME_ICON_PX / n)];
          if (px) cv->drawPixel(x + c, y + r, px);
        }
      }
      x += 6 * s;
      continue;
    }
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

// GPIO18 is on ADC2, which cannot be read while the radio is busy (ESP-NOW, Wi-Fi): a failed read comes back as 0.
// Failed samples are ignored; with none left the last good value stands, and batteryKnown tells whether there ever was one.
static bool batteryKnown = false;
static int lastGoodBatteryPct = 0;
int readBatteryPercentage() {
  long rawSum = 0; int good = 0;
  for(int i = 0; i < 16; i++) { int v = analogRead(BATTERY_ADC_PIN); if (v > 100) { rawSum += v; good++; } delay(2); }
  if (good == 0) return lastGoodBatteryPct;
  float analogVolts = ((float)rawSum / good) * (3.3 / 4095.0);
  float calculatedBatteryVolts = analogVolts * 2.0; 
  int pct = (int)((calculatedBatteryVolts - 3.4) / (4.2 - 3.4) * 100.0);
  lastGoodBatteryPct = max(0, min(pct, 100));
  batteryKnown = true;
  return lastGoodBatteryPct;
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
  int textX = PANEL_RES_X - (int)(elapsed / ALERT_MS_PER_PX);
  drawText(frame, textX, 12, alertText, alertColor, 3);
  if (alertUsesIcon) {                           // the icon rides in front of the text
    int iconX = textX - 19, iconY = 12;
    for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) {
      uint16_t px = alertIcon[r * 16 + c];
      if (px) frame->drawPixel(iconX + c, iconY + r, px);
    }
  }
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

// ---------------------------------------------------------------------------------------------
// Second board. The first board (source) relays everything it gets from the phone to the second one
// (follower) over ESP-NOW, so the phone keeps a single Bluetooth link: phone -> board 1 -> board 2.
// Both boards sit on a fixed Wi-Fi channel while linked, so a linked board no longer joins the home Wi-Fi.
// Linking: the app tells board 1 to look for a partner (60 s); holding BOOT for 2 s on board 2 makes it listen.
// The boards swap addresses and remember each other in flash.
// ---------------------------------------------------------------------------------------------
#define MIRROR_CHANNEL        1
#define MIRROR_PAIR_MS        60000
#define MIRROR_BEAT_MS        2000     // follower -> source heartbeat, source -> follower full state refresh
#define MIRROR_ONLINE_MS      7000
#define MIRROR_BOOT_PIN       0
#define MIRROR_PKT_STATE      1        // [1] + the phone's state packet
#define MIRROR_PKT_CONFIG     2        // [2] + a config command
#define MIRROR_PKT_BEAT       3        // [3, battery%] follower -> source
#define MIRROR_PKT_BEACON     4        // source looking for a partner (broadcast)
#define MIRROR_PKT_ACCEPT     5        // follower -> source: linked
#define MIRROR_PKT_UNLINK     6

enum MirrorRole { MIRROR_NONE, MIRROR_SOURCE, MIRROR_FOLLOWER };
static MirrorRole mirrorRole = MIRROR_NONE;
static uint8_t mirrorPeer[6] = {0};
static bool mirrorReady = false;                 // ESP-NOW is running
static bool mirrorPairing = false;               // source: sending beacons
static bool mirrorListening = false;             // follower: waiting for a beacon
static unsigned long mirrorWindowEnd = 0;
static unsigned long mirrorLastHeard = 0;
static uint8_t mirrorPeerBattery = 255;
static volatile bool mirrorPersistPending = false, mirrorAcceptPending = false, mirrorNoticePending = false, mirrorClearPending = false;
static const uint8_t mirrorBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void handleState(const uint8_t* rawData, size_t dataLength);
static void handleConfig(const uint8_t* data, size_t len, bool viaMirror);

static void mirrorAddPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;                       // the current channel
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

static void mirrorSend(const uint8_t *mac, uint8_t type, const uint8_t *data, size_t len) {
  if (!mirrorReady || len > 249) return;
  uint8_t buf[250];
  buf[0] = type;
  if (len) memcpy(buf + 1, data, len);
  for (int attempt = 0; attempt < 8; attempt++) {
    if (esp_now_send(mac, buf, len + 1) == ESP_OK) return;
    delay(3);                             // the send queue was full
  }
}

// Phone -> board 1 -> board 2.
static void mirrorForward(uint8_t type, const uint8_t *data, size_t len) {
  if (mirrorRole == MIRROR_SOURCE) mirrorSend(mirrorPeer, type, data, len);
}

static void mirrorOnReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < 1) return;
  const uint8_t *src = info->src_addr;
  uint8_t type = data[0];
  if ((type == MIRROR_PKT_BEACON && mirrorListening) || (type == MIRROR_PKT_ACCEPT && mirrorPairing)) {
    memcpy(mirrorPeer, src, 6);
    mirrorAddPeer(src);
    mirrorRole = (type == MIRROR_PKT_BEACON) ? MIRROR_FOLLOWER : MIRROR_SOURCE;
    mirrorListening = false; mirrorPairing = false;
    mirrorLastHeard = millis();
    mirrorPersistPending = true; mirrorNoticePending = true;
    if (type == MIRROR_PKT_BEACON) mirrorAcceptPending = true;
    return;
  }
  if (mirrorRole == MIRROR_NONE || memcmp(src, mirrorPeer, 6) != 0) return;     // only the linked partner is heard
  switch (type) {
    case MIRROR_PKT_STATE:  if (mirrorRole == MIRROR_FOLLOWER) { handleState(data + 1, len - 1); mirrorLastHeard = millis(); } break;
    case MIRROR_PKT_CONFIG: if (mirrorRole == MIRROR_FOLLOWER) { handleConfig(data + 1, len - 1, true); mirrorLastHeard = millis(); } break;
    case MIRROR_PKT_BEAT:   if (mirrorRole == MIRROR_SOURCE && len >= 2) { mirrorLastHeard = millis(); mirrorPeerBattery = data[1]; } break;
    case MIRROR_PKT_UNLINK: mirrorClearPending = true; break;
  }
}

static void mirrorStartRadio() {
  if (mirrorReady) return;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.setSleep(false);                   // a sleeping radio would miss packets from the other board
  esp_wifi_set_channel(MIRROR_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) { Serial.println("[mirror] ESP-NOW failed to start"); return; }
  esp_now_register_recv_cb(mirrorOnReceive);
  mirrorAddPeer(mirrorBroadcast);
  if (mirrorRole != MIRROR_NONE) mirrorAddPeer(mirrorPeer);
  mirrorReady = true;
  Serial.println("[mirror] radio ready");
}

static void mirrorStopRadio() {
  if (!mirrorReady) return;
  esp_now_deinit();
  mirrorReady = false;
}

static void mirrorSave() {
  prefs.begin("scoreboard", false);
  prefs.putUChar("mirrorRole", (uint8_t)mirrorRole);
  prefs.putBytes("mirrorPeer", mirrorPeer, 6);
  prefs.end();
}

static void mirrorLoad() {
  prefs.begin("scoreboard", true);
  uint8_t role = prefs.getUChar("mirrorRole", MIRROR_NONE);
  if (role <= MIRROR_FOLLOWER && prefs.getBytesLength("mirrorPeer") == 6) {
    mirrorRole = (MirrorRole)role;
    prefs.getBytes("mirrorPeer", mirrorPeer, 6);
  }
  prefs.end();
}

static void mirrorUnlink(bool tellPartner) {
  if (tellPartner && mirrorReady && mirrorRole != MIRROR_NONE) for (int i = 0; i < 3; i++) { mirrorSend(mirrorPeer, MIRROR_PKT_UNLINK, nullptr, 0); delay(20); }
  if (mirrorReady && mirrorRole != MIRROR_NONE) esp_now_del_peer(mirrorPeer);
  mirrorRole = MIRROR_NONE;
  mirrorPairing = false; mirrorListening = false;
  mirrorPeerBattery = 255;
  mirrorSave();
  Serial.println("[mirror] unlinked");
}

// [CMD_MIRROR, 1 = look for a partner | 2 = stop looking | 3 = unlink]
static void mirrorCommand(const uint8_t *data, size_t len) {
  uint8_t status = CFG_OK;
  if (len != 2) status = CFG_BAD_ARG;
  else if (data[1] == 1) {
    if (mirrorRole != MIRROR_NONE) status = CFG_BAD_ARG;      // unlink first
    else {
      if (!mirrorReady) mirrorStartRadio();
      mirrorListening = false; mirrorPairing = mirrorReady;
      mirrorWindowEnd = millis() + MIRROR_PAIR_MS;
      if (!mirrorReady) status = CFG_BAD_ARG;
    }
  }
  else if (data[1] == 2) mirrorPairing = false;
  else if (data[1] == 3) mirrorUnlink(true);
  else status = CFG_BAD_ARG;
  uint8_t resp[2] = { CMD_MIRROR, status };
  pConfigChar->setValue(resp, 2);
  pConfigChar->notify();
}

static bool mirrorOnline() { return mirrorRole == MIRROR_SOURCE && mirrorLastHeard != 0 && millis() - mirrorLastHeard < MIRROR_ONLINE_MS; }

// Called from loop().
static void mirrorService() {
  unsigned long now = millis();
  static unsigned long lastBeacon = 0, lastBeat = 0, bootDownAt = 0;
  static bool bootWasDown = false;

  if (mirrorClearPending) { mirrorClearPending = false; mirrorUnlink(false); }
  if (mirrorPersistPending) { mirrorPersistPending = false; mirrorSave(); Serial.println("[mirror] linked"); }
  if (mirrorAcceptPending) { mirrorAcceptPending = false; mirrorSend(mirrorPeer, MIRROR_PKT_ACCEPT, nullptr, 0); }
  if (mirrorNoticePending) {                    // both boards say so on the panel
    mirrorNoticePending = false;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    alertText = "LINKED"; alertColor = panelColors[COLOR_ALERT_GAME]; alertUsesIcon = false;
    alertStartMillis = now; alertActive = true;
    xSemaphoreGive(stateMutex);
  }

  // BOOT button: 2-5 s listens for a partner, 6 s or more unlinks.
  bool down = (digitalRead(MIRROR_BOOT_PIN) == LOW);
  if (down && !bootWasDown) bootDownAt = now;
  if (!down && bootWasDown) {
    unsigned long held = now - bootDownAt;
    if (held >= 6000 && mirrorRole != MIRROR_NONE) mirrorUnlink(true);
    else if (held >= 2000 && held < 6000 && mirrorRole != MIRROR_SOURCE) {
      if (mirrorRole == MIRROR_FOLLOWER) mirrorUnlink(true);
      mirrorStartRadio();
      mirrorListening = mirrorReady; mirrorPairing = false;
      mirrorWindowEnd = now + MIRROR_PAIR_MS;
    }
  }
  bootWasDown = down;

  if ((mirrorPairing || mirrorListening) && (long)(now - mirrorWindowEnd) >= 0) {
    mirrorPairing = false; mirrorListening = false;
    if (mirrorRole == MIRROR_NONE) mirrorStopRadio();       // nobody answered: radio off again
  }
  if (mirrorPairing && now - lastBeacon >= 400) { lastBeacon = now; mirrorSend(mirrorBroadcast, MIRROR_PKT_BEACON, nullptr, 0); }

  if (mirrorRole != MIRROR_NONE && mirrorReady && now - lastBeat >= MIRROR_BEAT_MS) {
    lastBeat = now;
    if (mirrorRole == MIRROR_FOLLOWER) {
      uint8_t battery = (uint8_t)readBatteryPercentage();
      if (!batteryKnown) battery = 255;          // no battery wired, or the reading keeps failing
      mirrorSend(mirrorPeer, MIRROR_PKT_BEAT, &battery, 1);
    } else if (hasReceivedState) {            // full refresh: the partner catches up after a lost packet or a late power-on
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      String names = homeName + "|" + awayName;
      uint8_t packet[8 + 2 * NAME_MAX + 4];
      size_t n = 0;
      packet[n++] = homeScore; packet[n++] = awayScore; packet[n++] = period; packet[n++] = brightness;
      packet[n++] = timerMin; packet[n++] = timerSec; packet[n++] = isTimerRunning ? 1 : 0; packet[n++] = totalPeriods;
      for (size_t i = 0; i < names.length() && n < sizeof(packet); i++) packet[n++] = (uint8_t)names[i];
      xSemaphoreGive(stateMutex);
      mirrorSend(mirrorPeer, MIRROR_PKT_STATE, packet, n);
    }
  }
}

// The state packet from the phone, or relayed by the first board when this one mirrors it.
static void handleState(const uint8_t* rawData, size_t dataLength) {
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

class FullControlCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        handleState(pCharacteristic->getData(), pCharacteristic->getLength());
        mirrorForward(MIRROR_PKT_STATE, pCharacteristic->getData(), pCharacteristic->getLength());
    }
};

// A config command from the phone, or relayed by the first board (viaMirror: no reply to send).
static void handleConfig(const uint8_t* data, size_t len, bool viaMirror) {
    if (len < 1) return;
    auto reply = [&](uint8_t cmd, uint8_t status) {
        if (viaMirror) return;
        uint8_t resp[2] = { cmd, status };
        pConfigChar->setValue(resp, 2);
        pConfigChar->notify();
    };

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
        case CMD_END_TEXT: {
            size_t textLen = len - 1;
            bool valid = textLen >= 1 && textLen <= ALERT_TEXT_MAX;
            for (size_t i = 1; valid && i < len; i++) { valid = (data[i] >= 0x20 && data[i] <= 0x7E); }
            if (!valid) { reply(CMD_END_TEXT, CFG_BAD_ARG); return; }
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            matchEndText = "";
            for (size_t i = 1; i < len; i++) { matchEndText += (char)data[i]; }
            xSemaphoreGive(stateMutex);
            reply(CMD_END_TEXT, CFG_OK);
            break;
        }
        case CMD_NAME_ICON: {
            if (len == 2 && data[1] < 4) {                       // clear the slot
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                memset(nameIcons[data[1]], 0, sizeof(nameIcons[0])); sceneDirty = true;
                xSemaphoreGive(stateMutex);
                reply(CMD_NAME_ICON, CFG_OK); break;
            }
            size_t rows = (len >= 3) ? (len - 3) / (NAME_ICON_PX * 2) : 0;
            if (len < 3 + NAME_ICON_PX * 2 || (len - 3) % (NAME_ICON_PX * 2) != 0 || data[1] > 3 || data[2] > NAME_ICON_PX - 1 || data[2] + rows > NAME_ICON_PX) {
                reply(CMD_NAME_ICON, CFG_BAD_ARG); return;
            }
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            for (size_t r = 0; r < rows; r++)
                for (int c = 0; c < NAME_ICON_PX; c++) {
                    const uint8_t *px = &data[3 + (r * NAME_ICON_PX + c) * 2];
                    nameIcons[data[1]][(data[2] + r) * NAME_ICON_PX + c] = ((uint16_t)px[0] << 8) | px[1];
                }
            sceneDirty = true;
            xSemaphoreGive(stateMutex);
            reply(CMD_NAME_ICON, CFG_OK);
            break;
        }
        case CMD_ALERT_ICON: {
            if (len == 1) { alertIconRows = 0; reply(CMD_ALERT_ICON, CFG_OK); break; }          // remove the icon
            size_t rows = (len >= 2) ? (len - 2) / 32 : 0;
            if (len < 34 || (len - 2) % 32 != 0 || data[1] > 15 || data[1] + rows > 16) { reply(CMD_ALERT_ICON, CFG_BAD_ARG); return; }
            for (size_t r = 0; r < rows; r++) {
                for (int c = 0; c < 16; c++) {
                    const uint8_t *px = &data[2 + r * 32 + c * 2];
                    alertIcon[(data[1] + r) * 16 + c] = ((uint16_t)px[0] << 8) | px[1];
                }
                alertIconRows |= (uint16_t)(1u << (data[1] + r));
            }
            reply(CMD_ALERT_ICON, CFG_OK);
            break;
        }
        case CMD_ALERT: {
            size_t textLen = len - 2;
            bool valid = len >= 3 && textLen <= ALERT_TEXT_MAX && (data[1] == 1 || data[1] == 2 || data[1] == 3);
            for (size_t i = 2; valid && i < len; i++) { valid = (data[i] >= 0x20 && data[i] <= 0x7E) || (data[1] == 3 && isIconChar((char)data[i])); }   // goal alerts may carry the team's emoji (bytes 1-4)
            if (!valid) { reply(CMD_ALERT, CFG_BAD_ARG); return; }
            bool isPeriod = (data[1] == 2);                       // goal alerts share the game start alert's color and switch
            if (!shown(isPeriod ? COLOR_ALERT_PERIOD : COLOR_ALERT_GAME)) { reply(CMD_ALERT, CFG_OK); return; }   // this alert is switched off
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            String incoming = "";
            for (size_t i = 2; i < len; i++) { incoming += (char)data[i]; }
            incoming.toUpperCase();
            if (alertActive && incoming == alertText && millis() - alertStartMillis < 3000) {   // same alert already playing (the board announced the end itself): do not restart it
                xSemaphoreGive(stateMutex);
                reply(CMD_ALERT, CFG_OK);
                break;
            }
            alertText = "";
            for (size_t i = 2; i < len; i++) { alertText += (char)data[i]; }
            alertText.toUpperCase();
            alertColor = panelColors[isPeriod ? COLOR_ALERT_PERIOD : COLOR_ALERT_GAME];
            alertUsesIcon = (data[1] == 3 && alertIconRows == 0xFFFF);
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

class ConfigCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        const uint8_t* data = pCharacteristic->getData();
        size_t len = pCharacteristic->getLength();
        if (len < 1) return;
        if (data[0] == CMD_MIRROR) { mirrorCommand(data, len); return; }
        if (data[0] == CMD_PANEL) {         // [CMD_PANEL, preset]: this board only, not relayed to the second one
            uint8_t resp[2] = { CMD_PANEL, (uint8_t)((len == 2 && data[1] < PANEL_PRESETS) ? CFG_OK : CFG_BAD_ARG) };
            pConfigChar->setValue(resp, 2); pConfigChar->notify();
            if (resp[1] == CFG_OK) { prefs.begin("scoreboard", false); prefs.putUChar("panelPreset", data[1]); prefs.end(); delay(400); ESP.restart(); }
            return;
        }
        handleConfig(data, len, false);
        switch (data[0]) {      // everything that changes what the panel shows goes to the second board too
            case CMD_SET_COLORS: case CMD_SET_VISIBILITY: case CMD_END_TEXT: case CMD_NAME_ICON: case CMD_ALERT_ICON: case CMD_ALERT:
                mirrorForward(MIRROR_PKT_CONFIG, data, len);
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

// The mirror radio (ESP-NOW) and Bluetooth share one radio: an idle-looking link still costs Bluetooth packets, which stalls a long transfer.
// So the mirror radio is switched off while a Bluetooth update runs and comes back when it ends or fails.
static bool wifiPausedForOta = false;

static void otaWifiPause() {
  if (wifiPausedForOta) return;
  wifiPausedForOta = true;
  mirrorStopRadio();
  Serial.println("[ble-ota] radio off for the update");
}

static void otaWifiResume() {
  if (!wifiPausedForOta) return;
  wifiPausedForOta = false;
  if (mirrorRole != MIRROR_NONE) mirrorStartRadio();
  Serial.println("[ble-ota] radio back on");
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
  Serial.printf("[ble-ota] failed: op %u status %u after %u of %u bytes\n", op, status, (unsigned)otaWritten, (unsigned)otaSize);
  if (otaState == FWU_STARTING || otaState == FWU_RECEIVING) Update.abort();
  otaState = FWU_IDLE;
  otaEndRequested = false; otaAbortRequested = false; otaOverflow = false;
  otaHead = otaTail = 0;
  otaNotifyData(op, status, nullptr, 0);
  sceneDirty = true;
  otaWifiResume();
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
    Serial.printf("[ble-ota] aborted (phone cancelled or link lost) after %u of %u bytes\n", (unsigned)otaWritten, (unsigned)otaSize);
    if (otaState == FWU_STARTING || otaState == FWU_RECEIVING) Update.abort();
    otaState = FWU_IDLE; otaAbortRequested = false; otaEndRequested = false; otaHead = otaTail = 0;
    sceneDirty = true;
    otaWifiResume();
    return;
  }
  if (now - otaLastActivity > OTA_IDLE_TIMEOUT_MS) { Serial.println("[ble-ota] nothing received for 20 s"); otaFail(OTA_OP_ABORT, OTA_ERR_TIMEOUT); return; }

  if (otaState == FWU_STARTING) {
    otaWifiPause();
    Serial.printf("[ble-ota] start: %u bytes, crc %08x\n", (unsigned)otaSize, (unsigned)otaExpectedCrc);
    if (Update.begin(otaSize)) {
      otaState = FWU_RECEIVING; otaLastActivity = millis();
      otaNotifyData(OTA_OP_START, OTA_OK, nullptr, 0);
    } else {
      Serial.printf("[ble-ota] Update.begin failed: %s\n", Update.errorString());
      otaState = FWU_IDLE; sceneDirty = true;
      otaNotifyData(OTA_OP_START, OTA_ERR_WRITE, nullptr, 0);
      otaWifiResume();
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
      if (Update.write(&otaRing[idx], n) != n) { Serial.printf("[ble-ota] flash write failed: %s\n", Update.errorString()); otaFail(OTA_OP_DATA, OTA_ERR_WRITE); return; }
      otaCrc = crc32Update(otaCrc, &otaRing[idx], n);
      otaWritten += n;
      __sync_synchronize();
      otaTail = otaTail + n;
      avail -= n;
      otaLastActivity = millis();
      static uint32_t lastLogged = 0;
      if (otaWritten - lastLogged >= 65536 || otaWritten == otaSize) { lastLogged = otaWritten; Serial.printf("[ble-ota] %u / %u bytes\n", (unsigned)otaWritten, (unsigned)otaSize); }
    }
    static unsigned long lastAckMillis = 0;
    if (otaWritten - otaLastAck >= OTA_ACK_EVERY || (otaWritten == otaSize && otaLastAck != otaWritten) || (now - lastAckMillis >= OTA_ACK_REPEAT_MS && otaHead == otaTail)) {
      otaLastAck = otaWritten; lastAckMillis = now;
      uint8_t ack[5] = { OTA_OP_ACK, (uint8_t)otaWritten, (uint8_t)(otaWritten >> 8), (uint8_t)(otaWritten >> 16), (uint8_t)(otaWritten >> 24) };
      pOtaChar->setValue(ack, sizeof(ack));
      pOtaChar->notify();
    }
    if (otaEndRequested && otaHead == otaTail) {
      if (otaWritten != otaSize)              otaFail(OTA_OP_END, OTA_ERR_SIZE);
      else if ((~otaCrc) != otaExpectedCrc)   otaFail(OTA_OP_END, OTA_ERR_CRC);
      else if (!Update.end(true))             otaFail(OTA_OP_END, OTA_ERR_WRITE);
      else {                                   // image is in place and verified: tell the phone, then boot into it
        Serial.println("[ble-ota] image verified, restarting");
        otaNotifyData(OTA_OP_END, OTA_OK, nullptr, 0);
        statusLED.setPixelColor(0, statusLED.Color(0, 200, 0)); statusLED.show();     // green: done
        xSemaphoreTake(stateMutex, portMAX_DELAY); otaWritten = otaSize; drawOtaScreen(); xSemaphoreGive(stateMutex);
        delay(2000);
        ESP.restart();
      }
    }
  }
}

static HUB75_I2S_CFG::shift_driver panelDriverFor(uint8_t preset) {
  static const HUB75_I2S_CFG::shift_driver drivers[6] = { HUB75_I2S_CFG::SHIFTREG, HUB75_I2S_CFG::FM6124, HUB75_I2S_CFG::FM6126A,
                                                          HUB75_I2S_CFG::ICN2038S, HUB75_I2S_CFG::MBI5124, HUB75_I2S_CFG::DP3246 };
  return drivers[(preset / 2) % 6];
}

// ---------------------------------------------------------------------------------------------
// Panel diagnose mode. Shows the current preset's name over test patterns: colour bands, a border and a diagonal.
// A short BOOT press saves the next preset and restarts; holding BOOT for 3 s leaves and restarts normally.
// What counts as right: four clean bands, a complete border and a straight diagonal, no flicker.
// ---------------------------------------------------------------------------------------------
static void runPanelDiagnose() {
  const uint16_t W = PANEL_RES_X, H = PANEL_RES_Y;
  uint16_t colors[4] = { dma_display->color565(255, 0, 0), dma_display->color565(0, 255, 0), dma_display->color565(0, 0, 255), dma_display->color565(255, 255, 255) };
  dma_display->clearScreen();
  for (int i = 0; i < 4; i++) dma_display->fillRect(i * (W / 4), 10, W / 4, H - 20, colors[i]);
  dma_display->drawRect(0, 0, W, H, dma_display->color565(255, 255, 0));
  dma_display->drawLine(1, 1, W - 2, H - 2, dma_display->color565(255, 0, 255));
  dma_display->setTextWrap(false);
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(255, 255, 255));
  dma_display->setCursor(3, 2);
  dma_display->printf("%u/%u %s", panelPreset + 1, PANEL_PRESETS, PANEL_PRESET_NAMES[panelPreset]);
  dma_display->setCursor(3, H - 8);
  dma_display->print("BOOT next  HOLD exit");
  Serial.printf("[panel] diagnose, preset %u: %s\n", panelPreset, PANEL_PRESET_NAMES[panelPreset]);
  while (digitalRead(0) == LOW) delay(10);          // the press that opened diagnose mode
  while (true) {
    statusLED.setPixelColor(0, ((millis() / 400) % 2) ? statusLED.Color(0, 100, 200) : statusLED.Color(0, 0, 0)); statusLED.show();
    if (digitalRead(0) == LOW) {
      unsigned long down = millis();
      while (digitalRead(0) == LOW && millis() - down < 3000) delay(10);
      bool held = (millis() - down >= 3000);
      if (!held) {
        prefs.begin("scoreboard", false); prefs.putUChar("panelPreset", (panelPreset + 1) % PANEL_PRESETS); prefs.putBool("diagNext", true); prefs.end();
      }
      delay(100); ESP.restart();
    }
    delay(20);
  }
}

void setup() {
  Serial.begin(115200);
  analogSetAttenuation(ADC_11db); 
  statusLED.begin(); statusLED.setBrightness(30); statusLED.show();

  prefs.begin("scoreboard", false);
  deviceBroadcastName = prefs.getString("devName", "Scoreboard_Beta");
  panelPreset = prefs.getUChar("panelPreset", 0);
  if (panelPreset >= PANEL_PRESETS) panelPreset = 0;
  bool diagnose = prefs.getBool("diagNext", false);      // set by a BOOT press in diagnose mode: show the next preset's pattern straight away
  if (diagnose) prefs.putBool("diagNext", false);
  prefs.end();

  // Diagnose window: a BOOT press in the first 2 s (white blink) opens the panel diagnose mode.
  // (Holding BOOT while powering on would start the ROM loader instead.)
  pinMode(0, INPUT_PULLUP);
  for (unsigned long t0 = millis(); millis() - t0 < 2000 && !diagnose; ) {
    statusLED.setPixelColor(0, ((millis() / 150) % 2) ? statusLED.Color(120, 120, 120) : statusLED.Color(0, 0, 0)); statusLED.show();
    diagnose = (digitalRead(0) == LOW);
    delay(10);
  }
  statusLED.setPixelColor(0, 0); statusLED.show();

  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN_1);
  mxconfig.gpio.r1 = R1_PIN; mxconfig.gpio.g1 = G1_PIN; mxconfig.gpio.b1 = B1_PIN;
  mxconfig.gpio.r2 = R2_PIN; mxconfig.gpio.g2 = G2_PIN; mxconfig.gpio.b2 = B2_PIN;
  mxconfig.gpio.a  = A_PIN;  mxconfig.gpio.b  = B_PIN;  mxconfig.gpio.c  = C_PIN; mxconfig.gpio.d = D_PIN; 
  mxconfig.gpio.e  = E_PIN;
  mxconfig.gpio.lat = LAT_PIN; mxconfig.gpio.oe = OE_PIN; mxconfig.gpio.clk = CLK_PIN;
  mxconfig.clkphase = (panelPreset % 2 == 1); mxconfig.driver = panelDriverFor(panelPreset);

  dma_display = new MatrixPanel_I2S_DMA(mxconfig); dma_display->begin();
  dma_display->setBrightness8(brightness);
  if (diagnose) runPanelDiagnose();     // never returns
  loadPanelColors();
  frame = new GFXcanvas16(PANEL_RES_X, PANEL_RES_Y);
  slotCanvas = new GFXcanvas16(NAME_SLOT_W, NAME_SLOT_H);
  stateMutex = xSemaphoreCreateMutex();
  marqueeSet(homeMarquee, homeName);
  marqueeSet(awayMarquee, awayName);
  drawScoreboard();

  pinMode(MIRROR_BOOT_PIN, INPUT_PULLUP);
  mirrorLoad();
  if (mirrorRole != MIRROR_NONE) mirrorStartRadio();      // linked boards talk to each other

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
  mirrorService();

  // Clock, scrolling names and redraw. The clock keeps counting here even if the phone link drops.
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (isTimerRunning && currentMillis - lastTickMillis >= 1000) {
    lastTickMillis += 1000;
    if (currentMillis - lastTickMillis > 2000) lastTickMillis = currentMillis;  // far behind: resync instead of catching up
    if (timerSec > 0) timerSec--;
    else if (timerMin > 0) { timerMin--; timerSec = 59; }
    else {
      isTimerRunning = false;
      if (hasReceivedState && period >= totalPeriods && shown(COLOR_ALERT_GAME)) {      // the last period ran out: announce it on the panel
        alertText = matchEndText; alertText.toUpperCase();
        alertColor = panelColors[COLOR_ALERT_GAME];
        alertUsesIcon = false;
        alertStartMillis = currentMillis;
        alertActive = true;
      }
    }
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

  static unsigned long lastNotifyMillis = 0;
  if (currentBLEState == LINKED_CONNECTED && currentMillis - lastNotifyMillis >= 2000) {
     lastNotifyMillis = currentMillis;
     batteryPct = readBatteryPercentage();
     
     // FIXED: Added array assignment tracking brackets to structure multi-byte list payloads
     uint8_t flags = (batteryPct < 15 ? FLAG_LOW_BATTERY : 0) | (pendingDeviceName.length() > 0 ? FLAG_RENAME_STAGED : 0)
                     | (mirrorRole == MIRROR_SOURCE ? FLAG_MIRROR_SOURCE : 0) | (mirrorOnline() ? FLAG_MIRROR_ONLINE : 0)
                     | (mirrorRole == MIRROR_FOLLOWER ? FLAG_MIRROR_FOLLOWER : 0) | (mirrorPairing ? FLAG_MIRROR_PAIRING : 0);
     uint8_t telemetryPacket[] = { (uint8_t)timerMin, (uint8_t)timerSec, (uint8_t)(isTimerRunning ? 1 : 0), (uint8_t)period, (uint8_t)batteryPct, flags, PROTOCOL_VERSION, FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH, (uint8_t)(mirrorOnline() ? mirrorPeerBattery : 255), panelPreset };
     pChar->setValue(telemetryPacket, sizeof(telemetryPacket)); pChar->notify();
  }

  if (currentMillis - lastBlinkMillis >= (otaState != FWU_IDLE ? 250 : 400)) {
    lastBlinkMillis = currentMillis;
    blinkToggleState = !blinkToggleState;

    if (otaState != FWU_IDLE) {                      // firmware update running: blink red until it is done
      statusLED.setPixelColor(0, blinkToggleState ? statusLED.Color(200, 0, 0) : statusLED.Color(0, 0, 0)); statusLED.show();
    }
    else if (mirrorPairing || mirrorListening) {     // looking for the second board: purple
      statusLED.setPixelColor(0, blinkToggleState ? statusLED.Color(150, 0, 200) : statusLED.Color(0, 0, 0)); statusLED.show();
    }
    else if (currentBLEState == LINKED_CONNECTED) {  // back to solid green (an update may have just ended or failed)
      statusLED.setPixelColor(0, statusLED.Color(0, 150, 0)); statusLED.show();
    }
    else if (currentBLEState == SEARCHING_NEW) {
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
