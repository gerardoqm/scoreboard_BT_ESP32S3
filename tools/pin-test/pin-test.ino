// Pin test: lights ONE colour data line of the HUB75 panel at a time, driven directly (no display library),
// so a dead wire or a dead panel input can be told apart from a software or driver problem.
//
// Every 4 seconds the sketch moves to the next line and fills the panel through it:
//   1 R1 top red       2 G1 top green     3 B1 top blue
//   4 R2 bottom red    5 G2 bottom green  6 B2 bottom blue      7 all six: white
// What you should see: a RED, GREEN or BLUE (or white) fill on the TOP half for 1 to 3 and on the BOTTOM half for 4 to 6.
// A step where nothing lights up means that line (the wire, the ESP32 pin or the panel input) is dead.
// Film the panel for about 30 seconds and note which steps show light.
// The Serial Monitor prints the step number too (115200 baud).
#include <Arduino.h>
#include <soc/gpio_reg.h>

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

#define COLUMNS 80
#define SCAN_ROWS 20          // 1/20 scan: the address lines A-E count 0..19, each row has a top and a bottom pixel

static const uint8_t DATA_PINS[6] = { R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN };
static const char *NAMES[7] = { "R1 top red", "G1 top green", "B1 top blue", "R2 bottom red", "G2 bottom green", "B2 bottom blue", "all six: white" };

static inline void high(uint32_t mask) { REG_WRITE(GPIO_OUT_W1TS_REG, mask); }
static inline void low(uint32_t mask)  { REG_WRITE(GPIO_OUT_W1TC_REG, mask); }

// The panel's driver chips need clock pulses of at least a few dozen nanoseconds; a bare register write is shorter than that.
#define PAUSE() do { asm volatile("nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop"); } while (0)

void setup() {
  Serial.begin(115200);
  const uint8_t all[] = { R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN, A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN };
  for (uint8_t pin : all) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  digitalWrite(OE_PIN, HIGH);      // outputs off (active low)
}

void loop() {
  static int step = -1;
  static unsigned long changed = 0;
  if (step < 0 || millis() - changed > 4000) {
    step = (step + 1) % 7;
    changed = millis();
    Serial.printf("[pin-test] step %d: %s\n", step + 1, NAMES[step]);
  }
  // the data bits for the chosen step
  uint32_t dataMask = 0;
  if (step == 6) { for (uint8_t pin : DATA_PINS) dataMask |= 1u << pin; }
  else dataMask = 1u << DATA_PINS[step];
  const uint32_t allData = (1u << R1_PIN) | (1u << G1_PIN) | (1u << B1_PIN) | (1u << R2_PIN) | (1u << G2_PIN) | (1u << B2_PIN);

  for (int row = 0; row < SCAN_ROWS; row++) {
    high(1u << OE_PIN);                                                    // blank while the row is loaded
    uint32_t addrHigh = 0, addrLow = 0;
    const uint8_t addr[5] = { A_PIN, B_PIN, C_PIN, D_PIN, E_PIN };
    for (int bit = 0; bit < 5; bit++) { if (row & (1 << bit)) addrHigh |= 1u << addr[bit]; else addrLow |= 1u << addr[bit]; }
    low(addrLow); high(addrHigh);
    low(allData); high(dataMask);                                          // the same bit for every column: the whole row fills
    PAUSE();
    for (int col = 0; col < COLUMNS; col++) { high(1u << CLK_PIN); PAUSE(); low(1u << CLK_PIN); PAUSE(); }
    high(1u << LAT_PIN); PAUSE(); low(1u << LAT_PIN); PAUSE();             // latch
    low(1u << OE_PIN);                                                     // show the row
    delayMicroseconds(200);
  }
}
