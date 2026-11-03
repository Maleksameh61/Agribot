# AgriBot firmware

There are 5 chips on this thing, so there are 5 sketches.

| Folder | Chip | What it does |
|---|---|---|
| `sensors/` | Sensors ESP32, the main one | WiFi + the phone page, missions, GPS, all the I2C sensors, soil probe, the log |
| `motion/` | Motion ESP32 | the 4 gearmotors through the MDD10As, the probe stepper, camera servos, the 2 HC-SR04s |
| `audio/` | Audio ESP32 | sounds off the microSD, texts through the SIM800L |
| `display/` | CYD | status screen and STOP / GO / PAUSE / PROBE / UP buttons |
| `camera/` | ESP32-CAM | video to the phone, and the Edge Impulse model when it's parked |

They talk to each other through `common/agrilink.h`. The Arduino IDE won't include anything from outside the sketch folder, so every sketch has its own copy. If you change it, change the one in `common/`, run `./sync_common.sh`, and flash all four boards again. If the copies don't match, the boards read each other wrong.

Everything compiles on ESP32 core 3.0.7 and it needs 3.x, because the audio uses the new I2S driver. None of it has run on the real board yet, since the boards aren't made.

I used AI to help write parts of this firmware as i t was so big and there are lots of arctechtuers so there are some parts in the code that needed ai to review and debug.

## What you need

Arduino IDE 2 with **esp32 by Espressif, 3.x**, plus these from the Library Manager:

- **ESPAsyncWebServer** and **AsyncTCP** by *ESP32Async*. Not the old me-no-dev ones, those don't build on core 3.
- **ArduinoJson** 7
- **TinyGPSPlus**
- **EspSoftwareSerial** (it pulls in *ghostl* too)
- **Adafruit BMP280** (pulls in Unified Sensor and BusIO)
- **LovyanGFX**, only for the CYD

Pick "ESP32 Dev Module" for Sensors, Motion, Audio and the CYD, and "AI Thinker ESP32-CAM" for the camera. Leave the partition scheme on default. Sensors is already at 89% of flash, so if I add much more it has to go to "Huge APP".

## Change these first

- `audio.ino` → `OWNER_NUMBER`. That's your number with +20, and it's the only number that can text it commands.
- `sensors.ino` and `camera.ino` → `AP_PASS`, and it has to be the same in both.
- `motion.ino` → `MICROSTEPS` to match the jumpers on the DRV8825, and `PINION_RADIUS_MM`.

## Sounds

The SD card is FAT32 with the files in the root. They're 16-bit PCM WAV (mono is fine), any sample rate up to 48 kHz. In Audacity that's Export → WAV → Signed 16-bit PCM.

`01` boot, `02` mission start, `03` got to a point, `04` probe going down, `05` reading saved, `06` something in front, `07` error, `08` mission done, `09` battery low, `10` horn

## How to use it

Join the **AgriBot** WiFi, open **192.168.4.1**, and add it to your home screen so it works like an app.

To set up a mission, drive it somewhere and press *ضيف مكاني دلوقتي*. Do that for every point, and untick "measure soil" if you just want it to drive through. The points stay saved after it's turned off. Then press *ابدأ*. *استنى* or *وقّف* pauses it, and *ابدأ* again carries on from the same point.

*نزّل كل القرايات* downloads `log.csv`. That's one line per soil reading with where and when it was taken, the weather, and what the camera saw.

If you'd rather type the points in, POST them to `/api/mission` like `[{"lat":30.0444,"lon":31.2357,"probe":1}]`.

You can text it `STATUS`, `GO` or `STOP`. It texts you by itself when a mission finishes, when it gives up (stuck, tipped over, lost GPS, dead battery, probe stuck) and when the battery gets low.

## Why it stops

- Motion stops the motors if it hears nothing from Sensors for 150 ms. Sensors sends from its own task every 50 ms, and if its main loop freezes for 200 ms it starts sending 0. So a frozen Sensors can't leave it driving.
- If the phone goes quiet for 300 ms, it stops.
- Motion won't drive forward if either ultrasonic sees something closer than 30 cm. It decides that on its own, and it can still back up and turn.
- The wheels won't move unless the probe is all the way up, so it can't drag the probe through the ground.
- Each chip has a watchdog, so a hung chip resets, and the 10k pull-downs keep the motor drivers off while it reboots.

There's no real e-stop (I explained why in the main README), so the deadman test below isn't optional.

## Testing it, wheels off the ground

1. **Motion by itself.** Flash it with Sensors unplugged. Nothing should move, and the serial monitor should show `flags 01`.
2. **Sensors + Motion.** Wait for "homing probe" to finish, which takes about 30 s the first time. Then drive it from the phone. Any wheel that spins the wrong way gets its `invert` flipped in `motors[]`.
3. **Deadman.** Hold full throttle and turn off the phone's WiFi. It has to stop in about a third of a second. Then do it again, but pull the power on Sensors this time.
4. **Probe.** Set the DRV8825 current limit first. Keep it low (around 0.8 to 1 A), because homing works by letting the stepper skip at the top. Lower the probe, measure how far it really went, and fix `PINION_RADIUS_MM` until 120 mm is actually 120 mm.
5. **Soil.** Put the probe in a pot of dirt and press *نزّل المجس واقرا*. If it says "soil probe not answering", check A/B on the MAX485 first, then the baud rate.
6. **Heading.** This one is outside. Drive straight for about 5 m and the heading should show a number. Then turn right in place. If the number goes down instead of up, set `YAW_SIGN` to `+1.0f`.
7. **First mission.** Two points 15 m apart, and walk next to it.
8. **Audio and SIM.** The Audio serial monitor should say `[sd] ok` and `[sim] ready`. Then text it `STATUS`.

## Stuff I guessed and still have to check

- **MD1 is the left side and MD2 is the right**, with the right side mirrored. If not, it's a quick fix in `motors[]`.
- **There's no limit switch on the probe.** Homing drives up 220 mm and the stepper skips against the top. It saves "probe is up" so it only does that again if the power dies mid-move. A microswitch on a spare pin would be better for the rack.
- **The DRV8825 FAULT pin needs a pull-up**, and GPIO34 doesn't have one. If it reads low at boot, Motion ignores it and says so.
- **Soil probe** is address 1, 4800 baud, registers 0 to 6 for moisture, temp, EC, pH, N, P and K. Two copies of the ZTS manual say the same thing. Neither one actually shows N/P/K in an example though.
- **The CYD cable goes on its P1** (VIN, TX, RX, GND). That's the same UART as its USB, so unplug it from the rover before you flash it.
- **AMP_SD comes straight off GPIO21.** That makes both MAX98357As play only the left channel. I send the same sound on both channels anyway, so it doesn't matter.
- **The BMP280 is at 0x77** (it tries 0x76 too), and **the BH1750 is at 0x23** with ADDR on GND.
- **The MPU6050 lies flat with Z up.** If it tilts past 25° the mission stops.
- **There's no compass.** It works out its heading from the gyro and fixes it from the GPS every 4 m it drives straight. At the start of a mission it has to drive straight for a few metres to figure out which way it's pointing, so give it some room.

## The UART thing

I only noticed this after the board was done. Sensors needs 5 serial links (Motion, Audio, GPS, soil probe, CYD), but it only has 2 free hardware UARTs, because UART0 is the USB. Motion and GPS get the real ones. Audio, the CYD and the probe run on software serial. They're slow and every message has a CRC, so a bad bit just loses one message, it doesn't give a wrong reading. If the Sensors log ever shows lots of `bad` on the CYD or Audio link, the fix for the next rev is to cut the CYD cable and put the CYD on WiFi like the camera.
