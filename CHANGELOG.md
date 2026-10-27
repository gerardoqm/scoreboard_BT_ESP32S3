# Firmware changelog

Versions follow `major.minor.patch` (see the Versions section of the README).

## 1.6.5
- Reliable delivery to the second board. What the first board relays now goes through a queue that sends one packet at a time and waits for the radio's verdict: a packet that was not acknowledged is sent again (up to 3 tries). A newer state replaces one still waiting, and a failed state is rebuilt from the board's current state, never resent as an old picture. A command that cannot be delivered (or is older than 1.5 s) makes the first board start a full sync. When the second board comes back after being away, it gets everything again. The first board never waits: its own panel is drawn at once.

## 1.6.4
- The second board now receives the commands the first board relays. The first board answered a command by overwriting the very buffer the command was in, and then relayed that buffer, so the second board got the reply instead of the command: goal, period and match alerts were rejected, and the emoji pictures of the team names arrived half loaded (cut in half). The commands (and the state) are copied before they are handled.

## 1.6.3
- The second board confirms that it has the picture: its heartbeat now says whether it has received a state. While it says no, the first board sends it everything it needs, rebuilt from its own memory (colors, visibility, the hundredths settings, the end text, the emoji pictures, then the state), again every 2 s until the second board confirms, so a board that powers up late or misses packets recovers by itself. The telemetry flags get bit 6, "the second board has the picture", which the app shows as "Mirroring" (and "Syncing" until then).
- Serial log (every 5 s) of the packets exchanged between the boards: sent and acknowledged, heard by type, to find out where a link breaks.

## 1.6.2
- Status LED of a linked second board: two green flashes every second while it is linked (it used to keep the blue "searching for a phone" blink, which was confusing). Unlinked, it goes back to the usual colors. A phone connected straight to it still shows solid green, and linking, updating and the purple "looking for a partner" blink keep their colors.
- A second board no longer wipes its display when a phone that was connected straight to it (for an update) goes away.

## 1.6.1
- The hundredths of a second are as big as the clock's digits (`00:41.36`, dropping the leading zeros when the period text is wide: `0:41.36`, `41.36`) and have their own color: new command `CMD_TICKER_COLOR` (0x0D, RGB565, saved in flash, red by default). Protocol version 12 as in 1.6.0.

## 1.6.0
- Hundredths of a second in the last minute: while the clock runs with less than a minute left, the panel shows it as `00:41.36`, in a small font after the clock in this first version. New command `CMD_TICKER` (0x0B, 0 or 1), saved in flash, on by default.
- A bigger goal icon: `CMD_ALERT_ICON_BIG` (0x0C) takes a 32 x 32 picture, drawn centered on the panel's height in front of the goal text (the 16 x 16 icon of `CMD_ALERT_ICON` remains as the fallback). Protocol version 12.
- The second board keeps its own clock phase when the first board refreshes it, so its seconds and hundredths no longer jump every two seconds.

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
