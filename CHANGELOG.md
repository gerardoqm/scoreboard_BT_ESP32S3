# Firmware changelog

Versions follow `major.minor.patch` (see the Versions section of the README).

## 1.5.1
- Diagnose mode: a short BOOT press now restarts straight into the next preset's test pattern (before, the board started normally and BOOT had to be pressed again within 2 seconds).

## 1.5.0
- Panel driver setting and diagnose mode. A HUB75 panel cannot tell the board which driver chip it has, so the right setting is found by eye. 12 presets (the six chips of the display library, each with the clock phase normal or inverted); preset 0 is what the firmware always used, so a board that is never changed behaves as before.
- Diagnose mode: press BOOT in the first 2 seconds after power-up (the LED blinks white). The panel shows the preset's name over colour bands, a border and a diagonal. A short BOOT press saves the next preset and restarts, holding BOOT for 3 seconds leaves. (Holding BOOT while powering on would start the ROM loader instead.)
- New command `CMD_PANEL` (0x0A): `[0x0A, preset]` saves the preset and restarts the board; the app's Panel type page uses it. Telemetry byte 11 is the current preset. Protocol version 11. The setting belongs to that board and is not sent to a linked second board.

## 1.4.1
- Wi-Fi networking removed: the board no longer joins a Wi-Fi network or runs `ArduinoOTA`, so `secrets.h` only needs `OTA_PASSWORD` (the Bluetooth update key) and no Wi-Fi password ends up in the firmware image. The board-to-board mirror (ESP-NOW) is unchanged. Update over Bluetooth or USB.

## Documentation fix
- Power: the diagram and the text now show a 5 A fuse and a 10 A switch in the load path, the charge board off the load path, the XL6009 fed from the battery (it feeds both the panel and the ESP32), and the voltage divider after the switch. A power-bank style board in the load path overheated and made the 5 V sag, which killed the green and blue LEDs first.
- Build setting: **PSRAM is now Disabled** in the documented compilation settings. The firmware never uses PSRAM, and with `OPI PSRAM` selected a board with a faulty PSRAM restarted in a loop at power-up (reset reason RTCWDT), whatever sketch it ran.
- New `tools/panel-test` sketch: colour bands repeated for each panel driver chip type, to diagnose wrong or missing colours.
- The HUB75 pinout was wrong for E and ground: E is on socket pin 8 and ground on pins 4 and 16 (as printed on the panel, `JIN`), not E on 16 and ground on 8. The diagram and the pin table are corrected, and the panel's 4-pin power header (2 VCC, 2 GND) is described. No firmware change: the GPIO numbers were right.

## 1.3.0
- The board announces the end of the match by itself: when the clock reaches zero in the last period it scrolls the text the app sent with the new `CMD_END_TEXT` ("FULL TIME" until then), even if the phone is gone. An identical alert from the phone a moment later does not restart it. Protocol version 9.

## 1.2.3
- Alert text can be up to 32 characters (was 16), so a goal alert fits a long team name. Protocol version 8.

## 1.2.2
- A goal alert can carry the scoring team's emoji (the same bytes 1-4 as the names): it is drawn 15 x 15 inside the scrolling text.

## 1.2.1
Bluetooth update reliability.
- Wi-Fi is switched off while a Bluetooth update runs (the two share one radio) and comes back when it ends or fails.
- The update prints its progress and any failure reason on the serial port.

## 1.2.0
Emoji in team names.
- New command CMD_NAME_ICON receives up to four 10x10 RGB565 pictures. A team name shows picture N where it holds the byte N (home: 1-2, away: 3-4), in line with the letters. A name that is too wide still scrolls.
- Protocol version 7. Includes everything in 1.1.0.

## 1.1.0
Goal alert with an icon.
- New command CMD_ALERT_ICON receives a 16x16 RGB565 picture (a few rows at a time). The new goal alert (CMD_ALERT type 3) scrolls it in front of its text, using the game start alert's color and switch.
- Protocol version 6.
- Includes everything in 1.0.2.

## 1.0.2
Bluetooth update fixes.
- A lost progress notification could freeze the transfer until it timed out. The board now repeats the progress ACK every 250 ms and every 2 KB.
- While an update runs the status LED blinks red; when the image is verified it turns green, and the board restarts by itself two seconds later.

## 1.0.1
Same as 1.0.0. This was the image bundled in the app so a Bluetooth update from 1.0.0 could be tested.

## 1.0.0
First numbered version.
- Layout: timer and period on top, scores at 3x, team names at 2x, 3x5 font, scrolling names.
- The board counts its own seconds, so the clock keeps running if the phone drops.
- Panel colors, start alerts, and hide or show for each element.
- Firmware updates over Bluetooth, with a progress bar on the panel.
- Reports its version in the telemetry (bytes 7-9).
