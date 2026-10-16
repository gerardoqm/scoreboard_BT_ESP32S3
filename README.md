# ESP32-S3 HUB75 P4 LED Matrix Scoreboard Firmware

This repository contains the resilient, optimized C++ firmware required to transform an **ESP32-S3 N16R8** development module into an ultra-responsive, pure display server driving an **80x40 P4 HUB75 LED Matrix Panel** with a demanding 1/20 scan rate multiplex architecture. 

## ✨ Key Features

- **Pure Display Slave Architecture:** All complex string formatting, language localizations, and clock cycles are handled by the paired iPhone application. The chip extracts layout control bytes and instantly draws what the app transmits, preventing processing lag.
- **Persistent Naming & Renaming:** Allows users to name their board dynamically from the smartphone remote (e.g., *"Juan's Board"*). The string is saved securely to non-volatile internal flash memory using the `Preferences` library and automatically updates the BLE broadcast identity profile.
- **Smart Connection Recovery & Timeout:** 
  - *Searching:* Onboard RGB NeoPixel blinks **Solid Blue**.
  - *Connected:* Transitions cleanly to **Solid Green**.
  - *Accidental Dropout:* Blinks **Cyan (Blue/Green Mix)** for 30 seconds while waiting to catch its previous companion phone. If the 30-second window expires, it wipes its display to zero parameters and opens the airwaves for a clean connection.
- **Analog Battery Monitoring:** Continuously samples raw analog cell voltages through an optimized 10-sample rolling average function on **GPIO 18**, translating the LiPo discharge curve safely into a `0%-100%` wireless notify data stream.
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

### Network Credentials (OTA Setup)
Enter your local router configurations directly into lines **17 and 18** of the code:
```cpp
const char* ssid = "YOUR_HOME_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";
```
Once flashed via USB-C, choose your board name under **Tools > Port > Network Ports** and provide the default security password **`1234`** to push future changes wirelessly!


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
