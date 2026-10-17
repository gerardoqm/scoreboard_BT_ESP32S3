#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Adafruit_GFX.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <ArduinoOTA.h> 

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
#define CFG_OK       0x00
#define CFG_BAD_ARG  0x01
#define CFG_NOTHING_STAGED 0x02
#define DEVICE_NAME_MAX 20

// Protocol version reported in the telemetry notification. Bump when the packet layout changes.
// Telemetry (5 bytes in v1): [min, sec, running, period, battery%]
// v2 adds:                    [flags, protocolVersion]  (7 bytes)
#define PROTOCOL_VERSION 2
#define FLAG_LOW_BATTERY    0x01  // battery below 15%
#define FLAG_RENAME_STAGED  0x02  // a name is staged and waiting for APPLY
#define PANEL_LABEL_MAX 13        // 80px panel / 6px per character

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
String periodRatio = "1/4";
String bottomClockString = "00:00";
String deviceBroadcastName = "Scoreboard_Beta";

BLEServer *pServer = nullptr;
BLECharacteristic *pChar = nullptr;
BLECharacteristic *pConfigChar = nullptr;
String pendingDeviceName = "";

int readBatteryPercentage() {
  long rawSum = 0;
  for(int i = 0; i < 10; i++) { rawSum += analogRead(BATTERY_ADC_PIN); delay(2); }
  float analogVolts = (rawSum / 10.0) * (3.3 / 4095.0);
  float calculatedBatteryVolts = analogVolts * 2.0; 
  int pct = (int)((calculatedBatteryVolts - 3.4) / (4.2 - 3.4) * 100.0);
  return max(0, min(pct, 100));
}

void drawScoreboard() {
  dma_display->fillScreen(dma_display->color565(0, 0, 0));
  dma_display->drawRect(0, 0, 80, 40, dma_display->color565(0, 180, 0)); 
  dma_display->drawFastVLine(40, 0, 26, dma_display->color565(40, 40, 40)); 
  dma_display->setTextSize(1);
  
  dma_display->setTextColor(dma_display->color565(0, 120, 255));
  dma_display->setCursor(3, 3); dma_display->print(homeName);
  dma_display->setCursor(14, 14); dma_display->print(String(homeScore));

  dma_display->setTextColor(dma_display->color565(255, 40, 40));
  dma_display->setCursor(44, 3); dma_display->print(awayName);
  dma_display->setCursor(55, 14); dma_display->print(String(awayScore));

  dma_display->drawRect(31, 0, 19, 11, dma_display->color565(80, 80, 80));
  dma_display->setTextColor(dma_display->color565(255, 255, 0)); 
  dma_display->setCursor(33, 2); dma_display->print(periodRatio);

  dma_display->drawFastHLine(0, 26, 80, dma_display->color565(80, 80, 80));

  if (isTimerRunning) {
    dma_display->setTextColor(dma_display->color565(0, 255, 0));   
  } else {
    dma_display->setTextColor(dma_display->color565(255, 120, 0)); 
  }
  dma_display->setTextWrap(false);
  dma_display->setCursor(2, 30);
  dma_display->print(bottomClockString.substring(0, PANEL_LABEL_MAX));
}

class ScoreboardServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { currentBLEState = LINKED_CONNECTED; statusLED.setPixelColor(0, statusLED.Color(0, 150, 0)); statusLED.show(); }
    void onDisconnect(BLEServer* pServer) {
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
            // FIXED: Added precise index tracking matrix brackets to extract individual bytes
            hasReceivedState = true;
            homeScore      = rawData[0];
            awayScore      = rawData[1];
            period         = rawData[2];
            brightness     = constrain(rawData[3], 10, 120); // same range as the app's slider
            timerMin       = rawData[4];
            timerSec       = rawData[5];
            isTimerRunning = (rawData[6] == 1);
            totalPeriods   = rawData[7];
            
            dma_display->setBrightness8(brightness);

            if (dataLength > 8) {
                String payload = "";
                for (size_t i = 8; i < dataLength; i++) { payload += (char)rawData[i]; }
                int pipe1 = payload.indexOf('|');
                int pipe2 = payload.indexOf('|', pipe1 + 1);
                int pipe3 = payload.indexOf('|', pipe2 + 1);
                
                if (pipe1 != -1 && pipe2 != -1) {
                    homeName = payload.substring(0, pipe1).substring(0, 4);
                    awayName = payload.substring(pipe1 + 1, pipe2).substring(0, 4);
                    if (pipe3 != -1) {
                        periodRatio = payload.substring(pipe2 + 1, pipe3).substring(0, 3);
                        // Field 4 is the bottom-line label (e.g. "1st Period 10:00"), never a device name.
                        bottomClockString = payload.substring(pipe3 + 1).substring(0, 31);
                    } else { periodRatio = payload.substring(pipe2 + 1); }
                }
            }
            drawScoreboard();
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
  pService->start();
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  
  currentBLEState = SEARCHING_NEW;
}

void loop() {
  unsigned long currentMillis = millis();

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
     uint8_t telemetryPacket[] = { (uint8_t)timerMin, (uint8_t)timerSec, (uint8_t)(isTimerRunning ? 1 : 0), (uint8_t)period, (uint8_t)batteryPct, flags, PROTOCOL_VERSION };
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
        hasReceivedState = false;
        homeScore = 0; awayScore = 0; period = 1; homeName = "HOME"; awayName = "AWAY"; periodRatio = "1/4"; bottomClockString = "00:00";
        drawScoreboard();
      }
    }
  }
  delay(10);
}
