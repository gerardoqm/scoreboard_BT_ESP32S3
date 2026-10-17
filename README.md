# ESP32-S3 HUB75 P4 LED Matrix Scoreboard Firmware

This repository contains the resilient, optimized C++ firmware required to transform an **ESP32-S3 N16R8** development module into an ultra-responsive, pure display server driving an **80x40 P4 HUB75 LED Matrix Panel** with a demanding 1/20 scan rate multiplex architecture. 

## ✨ Key Features

- **Pure Display Slave Architecture:** All complex string formatting, language localizations, and clock cycles are handled by the paired iPhone application. The chip extracts layout control bytes and instantly draws what the app transmits, preventing processing lag.
- **Persistent Naming & Renaming:** Allows users to name their board dynamically from the smartphone remote (e.g., *"Juan's Board"*). The string is saved securely to non-volatile internal flash memory using the `Preferences` library and automatically updates the BLE broadcast identity profile.
- **Smart Connection Recovery & Timeout:** 
  - *Searching:* Onboard RGB NeoPixel blinks **Solid Blue**.
  - *Connected:* Transitions cleanly to **Solid Green**.
  - *Accidental Dropout:* Blinks **Cyan (Blue/Green Mix)** for 30 seconds while waiting to catch its previous companion phone. If the 30-second window expires, it wipes its display to zero parameters and opens the airwaves for a clean connection.
- **Analog Battery Monitoring:** Continuously samples raw analog cell voltages through an optimized 10-sample rolling average function on **GPIO 18**, scaling the cell voltage linearly from 3.4 V (0%) to 4.2 V (100%) and sending it in the BLE telemetry notification. The divider halves the voltage, so the firmware doubles the reading.
- **Silent Network IDE OTA Programming:** Runs a background `ArduinoOTA` server. After the initial wire flash, your board registers wirelessly inside your Arduino IDE as a **Network Port**, allowing you to update the board over Wi-Fi without un-boxing the enclosure housing.

## 🔌 Hardware Pin Mapping Blueprint

Connect your 16-pin HUB75 display ribbon cable over to your ESP32-S3 development board using this safe, non-restricted GPIO matrix map:

| HUB75 Ribbon Connector Pin | Pin Function | Target ESP32-S3 GPIO Pin |
| :--- | :--- | :--- |
| Pin 1 | **R1** (Top Red Data) | **GPIO 1** |
| Pin 2 | **G1** (Top Green Data) | **GPIO 2** |
| Pin 3 | **B1** (Top Blue Data) | **GPIO 3** |
| Pin 5 | **R2** (Bottom Red Data) | **GPIO 4** |
| Pin 6 | **G2** (Bottom Green Data) | **GPIO 5** |
| Pin 7 | **B2** (Bottom Blue Data) | **GPIO 6** |
| Pin 9 | **A** (Row Select Line A) | **GPIO 7** |
| Pin 10 | **B** (Row Select Line B) | **GPIO 8** |
| Pin 11 | **C** (Row Select Line C) | **GPIO 9** |
| Pin 12 | **D** (Row Select Line D) | **GPIO 10** |
| Pin 16 | **E** (5th Row Select Line) | **GPIO 11** |
| Pin 14 | **LAT / STB** (Latch Data) | **GPIO 12** |
| Pin 15 | **OE** (Output Enable) | **GPIO 13** |
| Pin 13 | **CLK** (Clock Signal) | **GPIO 14** |
| Pins 4, 8, 20 | **GND** (Ground Reference) | **Any Board GND Pin** |

### Onboard Status Indicator
- **Onboard WS2812B RGB NeoPixel:** Driven natively on **GPIO 48**.

### Resistor Divider Telemetry (Battery Tracker)
- Solder two matching **10kΩ resistors** in series between your raw battery positive (`BAT+`) and Ground (`GND`). Tap a wire from the middle junction point directly to **GPIO 18**.

## ⚙️ Arduino IDE Compilation Configuration

To prevent flash memory restriction errors (`[Errno 32] Broken Pipe`), apply these exact board setup values under the **Tools** menu before compilation:

- **Board:** `ESP32S3 Dev Module`
- **Flash Size:** `16MB (128Mb)`
- **Partition Scheme:** `16MB Flash (2MB APP/12.5MB FATFS)`
- **PSRAM:** `OPI PSRAM`
- **Upload Mode:** `UART0 / Hardware CDC`
- **USB CDC On Boot:** `Enabled`

### Network Credentials & OTA Setup
Wi-Fi and OTA credentials are kept out of the sketch in a **gitignored** `secrets.h`:

1. Copy `secrets.h.example` to `secrets.h` (same folder as the `.ino`).
2. Fill in `WIFI_SSID`, `WIFI_PASSWORD` and choose your own `OTA_PASSWORD`.
3. Flash once over USB-C. Afterwards pick the board under **Tools > Port > Network Ports** and enter your OTA password to update wirelessly.

`secrets.h` is listed in `.gitignore`; never commit it.

## 📡 Bluetooth LE Protocol

One service, two characteristics. Keep these tables in sync with `AdvancedBLEManager.swift` in the BLEScoreboard app.

| Item | UUID |
| :--- | :--- |
| Service | `12345678-1234-1234-1234-123456789012` |
| State characteristic (read / write / notify) | `87654321-4321-4321-4321-210987654321` |
| Config characteristic (write / notify) | `87654321-4321-4321-4321-210987654322` |

### State: phone → board (write, 8 bytes + optional text)

| Byte | Field | Notes |
| :--- | :--- | :--- |
| 0 | Home score | 0-255 (the app limits it to 0-99) |
| 1 | Away score | same |
| 2 | Period | |
| 3 | Brightness | clamped by the firmware to 10-120 |
| 4 | Timer minutes | |
| 5 | Timer seconds | |
| 6 | Timer running | `1` = running |
| 7 | Total periods | |
| 8+ | Text (optional) | ASCII `home\|away\|ratio\|bottomLabel` |

Names are cut to 4 characters, the ratio to 3, and the bottom label to 13 (the width of the panel). The text must be plain ASCII because the panel font has no accents; the app strips them. Writes shorter than 8 bytes are ignored.

### Telemetry: board → phone (notify every 2 s while connected)

| Byte | Field | Notes |
| :--- | :--- | :--- |
| 0 | Timer minutes | |
| 1 | Timer seconds | |
| 2 | Timer running | |
| 3 | Period | |
| 4 | Battery % | 0-100 |
| 5 | Flags | bit 0 = battery below 15%, bit 1 = a new name is staged |
| 6 | Protocol version | currently `2`; bytes 5-6 are absent on version 1 firmware |

The app shows "firmware outdated" when it receives a shorter packet or a lower version than it expects. Bump `PROTOCOL_VERSION` in the sketch and `expectedProtocolVersion` in the app together whenever a layout changes.

### Config: renaming the board

Write `[command, args...]` to the config characteristic; each command is answered by a `[command, status]` notification (status `0` = OK, `1` = bad argument, `2` = nothing staged).

| Command | Bytes | Effect |
| :--- | :--- | :--- |
| `0x01` Set name | `01` + 1-20 printable ASCII bytes | Stages the name; nothing is saved yet |
| `0x02` Apply | `02` | Saves the staged name to flash and reboots |

### Connection states
After the first state packet of a session, an unexpected disconnect starts a 30 second cyan "retrying" window for the same phone. If no state was received yet, or the window expires, the board returns to blue "searching" and resets the display.

 🔋 CORE POWER & TELEMETRY HUB BLUEPRINT
 ───────────────────────────────────────
                                    ┌──────────────────────────────────┐
                                    │  10kΩ Resistor Divider Circuit   │
                                    │                                  │
                                    │ [LiPo BAT+] ───[ 10kΩ Resistor ] │
                                    │                        │         │
                                    │                        ├───► GPIO 18 (ADC Input)
                                    │                        │         │
                                    │  [ Board GND ] ──[ 10kΩ Resistor ] │
                                    └──────────────────────────────────┘
                                                          │
                                                          ▼
 [ Raw 3.7V - 4.2V Lithium Ion Battery Cell ] ────────────┴─────────────────────┐
           │                                                                    │
           ├───► [ TP4056 Boost Board ] ──► (Stable 5.0V VOUT) ──► To ESP32-S3  │
           │                                                       5V Power Pin │
           │                                                                    │
           └───► [ Round Volt/Amp Gauge ] ──► (Casing Exterior Monitor Display)  │
                                                                                │
 └──────────────────────────────────────────────────────────────────────────────┘
           │
           └───► [ XL6009 Boost Module ] ──► (Calibrated 5.0V VCC) ──► To P4 Panel
                                                                       Screw Blocks
                                                                             │
     ┌───────────────────────────────────────────────────────────────────────┘
     ▼
 🎨 HUB75 DATA DATA BUS SIGNAL PATH
 ──────────────────────────────────
 Look directly into the back socket of your P4 panel (numbered 1-16, top-left to bottom-right):

                      HUB75 Connector Pin Mapping
                     +---------------------------+
          R1 (Red)   |  (1)  [GPIO 1]   (2)  [GPIO 2]  | G1 (Green)
          B1 (Blue)  |  (3)  [GPIO 3]   (4)  [GND]     | GND (Reference)
          R2 (Red)   |  (5)  [GPIO 4]   (6)  [GPIO 5]  | G2 (Green)
          B2 (Blue)  |  (7)  [GPIO 6]   (8)  [GND]     | GND (Reference)
          A (Line)   |  (9)  [GPIO 7]   (10) [GPIO 8]  | B (Line)
          C (Line)   |  (11) [GPIO 9]   (12) [GPIO 10] | D (Line)
          CLK (Clock)|  (13) [GPIO 14]  (14) [GPIO 12] | LAT (Latch Data)
          OE (Enable)|  (15) [GPIO 13]  (16) [GPIO 11] | E (1/20 Line Select)
                     +---------------------------+
                                         │
                                         ▼
                   🚨 CRITICAL MAPPING CHECKPOINT S3 🚨
       Ensure an explicit jumper joins an ESP32 GND node directly to a 
       HUB75 GND pin or the screen graphics will render as scrolling noise!

///

## 🔋 Battery Sizing

> These figures are estimates for an 80x40 P4 panel running at the app's brightness limit. Measure your own build (see below) before buying a pack.

### How much power does it use?

| Part | Typical draw at 5 V |
| :--- | :--- |
| P4 80x40 panel (mostly dark, text only, brightness capped at 120/255) | 0.8 - 1.5 A |
| ESP32-S3 with Bluetooth and Wi-Fi | 0.1 - 0.2 A |

The boost converters lose about 15%, so the battery supplies roughly **5 - 9 W**, or about **1.4 - 2.5 A at 3.7 V**. Bright, full-white content draws more.

### Recommended pack

A **single-cell 3.7 V pack of 8,000 - 10,000 mAh with a protection circuit** covers a full day of games with breaks and setup time. Three protected 18650 cells in parallel (about 9,000 mAh) work well. 5,000 mAh is the minimum if you run only one or two games per charge.

| Pack | Approximate runtime |
| :--- | :--- |
| 5,000 mAh | 1.7 - 3 h |
| 8,000 mAh | 2.8 - 5 h |
| 10,000 mAh | 3.5 - 6 h |

Runtimes assume 85% of the rated capacity is usable and a 5 - 9 W draw.

### Measure it yourself

Run a normal game at your usual brightness for about 10 minutes and read the average current from the round volt/amp gauge. Then:

```
capacity needed (Ah) = average current (A) x hours wanted / 0.8
```

### Things to watch

- **Use a protected pack** (or a BMS) and put a fuse close to the battery. Do not use bare, unprotected LiPo cells.
- **Charging is slow.** The TP4056 charges at 1 A at most, so a 10 Ah pack needs 10 hours or more. For packs above about 6 Ah, consider a faster charger.
- **The XL6009 that feeds the panel** is rated for 4 A of switch current. At 3.7 V in, a 1.5 A panel load already needs about 2.4 A from the battery, so expect it to get warm and check that the panel does not spike above that at full white.
- **The battery percentage is approximate.** The firmware scales 3.4 V (0%) to 4.2 V (100%) in a straight line, while a real LiPo discharge curve is not linear. Use it as a rough gauge, not a fuel meter.
