# Firmware changelog

Versions follow `major.minor.patch` (see the Versions section of the README).

## 1.3.0 (not released)
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
