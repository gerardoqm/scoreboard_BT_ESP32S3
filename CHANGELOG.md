# Firmware changelog

Versions follow `major.minor.patch` (see the Versions section of the README).

## 1.0.1 (not released)
Same as 1.0.0. This is the image bundled in the app so a Bluetooth update from 1.0.0 can be tested end to end.

## 1.0.0
First numbered version.
- Layout: timer and period on top, scores at 3x, team names at 2x, 3x5 font, scrolling names.
- The board counts its own seconds, so the clock keeps running if the phone drops.
- Panel colors, start alerts, and hide or show for each element.
- Firmware updates over Bluetooth, with a progress bar on the panel.
- Reports its version in the telemetry (bytes 7-9).
