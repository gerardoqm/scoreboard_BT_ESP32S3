// Panel test: colour bars on the 80x40 HUB75 panel, repeated with each panel driver chip type the library supports.
//
// Use it when the panel shows wrong or missing colours, faded areas or scrambled rows. It does not use Bluetooth or Wi-Fi.
//   1. Flash it over USB (same board settings as the scoreboard sketch).
//   2. Film the panel for about a minute. The board restarts every 8 seconds and tries the next driver type;
//      the number of white dots in the bottom-left corner says which one it is:
//      1 SHIFTREG (plain, what the scoreboard uses), 2 FM6124, 3 FM6126A, 4 ICN2038S, 5 MBI5124, 6 DP3246.
//   3. The driver type that shows clean bars is the one the scoreboard sketch must use.
//
// What the picture should look like (full brightness on top, half brightness below):
//   four vertical bands, left to right: RED, GREEN, BLUE, WHITE.
//   A missing colour in a band, or a band that fades along the width, points at a data wire (R1 G1 B1 top half, R2 G2 B2 bottom half),
//   a clock problem (long wires, loose connector) or the wrong driver type.
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Preferences.h>

#define PANEL_RES_X 80
#define PANEL_RES_Y 40

// Same wiring as the scoreboard sketch.
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

static const HUB75_I2S_CFG::shift_driver DRIVERS[] = {
  HUB75_I2S_CFG::SHIFTREG, HUB75_I2S_CFG::FM6124, HUB75_I2S_CFG::FM6126A,
  HUB75_I2S_CFG::ICN2038S, HUB75_I2S_CFG::MBI5124, HUB75_I2S_CFG::DP3246
};
static const char *NAMES[] = { "SHIFTREG", "FM6124", "FM6126A", "ICN2038S", "MBI5124", "DP3246" };
static const int DRIVER_COUNT = sizeof(DRIVERS) / sizeof(DRIVERS[0]);

MatrixPanel_I2S_DMA *panel = nullptr;
Preferences prefs;

void setup() {
  Serial.begin(115200);
  delay(2500);                                             // time to open the serial monitor
  Serial.printf("[panel-test] reset reason %d, free heap %u, free psram %u\n", (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  prefs.begin("paneltest", false);
  int index = prefs.getInt("index", 0) % DRIVER_COUNT;
  prefs.putInt("index", (index + 1) % DRIVER_COUNT);        // the next restart tries the next driver
  prefs.end();
  Serial.printf("[panel-test] driver %d of %d: %s\n", index + 1, DRIVER_COUNT, NAMES[index]);

  HUB75_I2S_CFG cfg(PANEL_RES_X, PANEL_RES_Y, 1);
  cfg.gpio.r1 = R1_PIN; cfg.gpio.g1 = G1_PIN; cfg.gpio.b1 = B1_PIN;
  cfg.gpio.r2 = R2_PIN; cfg.gpio.g2 = G2_PIN; cfg.gpio.b2 = B2_PIN;
  cfg.gpio.a = A_PIN; cfg.gpio.b = B_PIN; cfg.gpio.c = C_PIN; cfg.gpio.d = D_PIN; cfg.gpio.e = E_PIN;
  cfg.gpio.lat = LAT_PIN; cfg.gpio.oe = OE_PIN; cfg.gpio.clk = CLK_PIN;
  cfg.clkphase = false;
  cfg.driver = DRIVERS[index];

  panel = new MatrixPanel_I2S_DMA(cfg);
  bool ok = panel->begin();
  Serial.printf("[panel-test] panel->begin() returned %s\n", ok ? "true" : "false");
  if (!ok) { delay(4000); ESP.restart(); }
  panel->setBrightness8(80);
  panel->clearScreen();

  // four bands, 20 pixels wide: red, green, blue, white. Top half at full level, bottom half at half level.
  const uint8_t level[2] = { 255, 128 };
  for (int half = 0; half < 2; half++) {
    int y0 = half * 20;
    uint8_t v = level[half];
    for (int band = 0; band < 4; band++) {
      uint16_t color = panel->color565(band == 0 || band == 3 ? v : 0, band == 1 || band == 3 ? v : 0, band == 2 || band == 3 ? v : 0);
      panel->fillRect(band * 20, y0, 20, 20, color);
    }
  }
  // the driver number: that many white dots in the bottom-left corner, on a black strip
  panel->fillRect(0, 36, 14, 4, 0);
  for (int i = 0; i <= index; i++) panel->drawPixel(1 + i * 2, 38, panel->color565(255, 255, 255));
  Serial.println("[panel-test] bars drawn");
}

void loop() {
  delay(8000);
  ESP.restart();
}
