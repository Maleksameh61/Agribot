# Printables (STEP)

3D-printed parts for AgriBot, ready to slice. All files are STEP, in mm.

## Motor mounts (print 4)

| File | What it is |
|---|---|
| `motor-mount-1.step` … `motor-mount-4.step` | Mounts the four 5840-31ZY worm gearmotors sit on (96.2 × 100 × 17.1 mm each) |

Exported from the AgriBot Fusion 360 model (bodies Body101–Body104).

## Camera pan/tilt (ESP32-CAM + 2× MG90S)

| File | What it is | Size (mm) |
|---|---|---|
| `pan-tilt-yaw-base.step` | Pan (yaw) base, holds the pan servo | 71.2 × 65.8 × 28.2 |
| `pan-tilt-tilt-plate.step` | Tilt plate | 52.4 × 52.4 × 31.9 |
| `pan-tilt-cam-case-with-tilt-support.step` | ESP32-CAM case with tilt support | — |
| `pan-tilt-cam-back-cover.step` | ESP32-CAM back cover | 31.1 × 44.0 × 3.5 |

Original print settings: 0.3 mm layers, 0.4 mm nozzle, PETG, about 3.25 h and 55 g total.

These were converted from the original STLs, so they're faceted solids (triangle faces) and not
parametric CAD. They print exactly like the STLs.

**Credit / license:** "ESP32 Cam Pan and Tilt Camera" by werz 99 on Printables
(https://www.printables.com/model/148269), based on "Camera mount MG90s Tilt and Yaw (Arduino ESP32-Cam)"
by Electronlibre on Thingiverse. Licensed under CC BY-NC-SA 4.0. These pan/tilt files keep that license
(non-commercial, share-alike, with attribution), whatever license covers the rest of this repo.
