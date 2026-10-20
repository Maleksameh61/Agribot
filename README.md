# The AgriBot rover

![AgriBot board, 3D render](images/3d-model.png)

[![View PCB in KiCanvas](https://img.shields.io/badge/PCB-KiCanvas-blue)](https://kicanvas.org/?github=https://github.com/Maleksameh61/Agribot/tree/main/PCB)

AgriBot tops out at 1.4 km/h, so you can outwalk it, and that's fine, because its job is to drive to a GPS point, stop, push a soil probe into the ground and read moisture, temperature, pH, salinity (EC) and NPK, then go do it again somewhere else.

This repo is the board that runs all of that: three ESP32s, five buck converters, the motor driver links, the stepper for the probe, GPS, a SIM800L and two speakers, on 260 x 150 mm of 2 oz copper.

## How it works

The idea is that nobody drives it most of the time. You give it a list of GPS points and it goes to each one, with two HC-SR04s up front so it doesn't drive into a tree. When it gets there it stops, and a NEMA 17 on a rack and pinion pushes the RS485 soil probe down. Sensors reads the probe and saves the numbers with the GPS position. While it's parked the ESP32-CAM can look at the crops, and if it's too far out for WiFi, the Audio ESP32 texts me through the SIM800L.

You can also drive it from your phone, mostly to get it to the field or out of a ditch. The phone joins the WiFi that Sensors puts up and sends throttle and steering about 20 times a second. Sensors passes that down a UART to Motion, and Motion turns it into left and right speeds for the two Cytron MDD10A drivers. Video goes straight from the ESP32-CAM to your phone over the same WiFi, and two servos aim the camera.

Motion only moves while Sensors keeps talking to it. If 150 ms go by without a command, the motors stop, and that's the entire safety system. Yes, I thought about a real e-stop. At walking pace you can just catch it, and pulling an XT60 kills everything.

Sensors also reads temperature, humidity, pressure, light and an MPU6050, and shows what's going on on a 2.8" Cheap Yellow Display over a 4-pin cable. Audio plays sounds off a microSD through two speakers.

## Why I made it

Salty soil is the big problem on small farms in Egypt, and checking a field means walking it with a probe or sending samples to a lab. I wanted a rover to do the walking. I also wanted every part to come from shops in Cairo, and the whole board, PCB and shipping included, comes to about $141.

I took AgriBot to RobotChallenge in Beijing this August and came 4th in FreeStyle Senior. After that I redid the electronics. The plan was three separate boards (Motion, Sensors, Audio) talking over ESP-NOW, and I didn't like the chip that drives the motors depending on radio, so now it's one board with the three ESP32s wired together over UART and Sensors in the middle. Before you ask why there are still three: I counted about 39 GPIOs for everything on here, and a DevKit gives you about 24 you can actually use.

## Pictures

These are renders of the real board file. The boards aren't ordered yet, so no photos.

### PCB layout
Red is top copper, blue is bottom. The big red areas are the 12 V pour and the bottom is almost all ground.

![PCB layout](images/pcb.png)

### Front and back

![Front of the board](images/pcb-front.png)

![Back of the board](images/pcb-back.png)

### The three ESP32s
Motion, Sensors and Audio, left to right. Sensors is the only one with its WiFi on.

![The three ESP32s](images/three-esp32s.png)

### Schematic

![Full schematic](images/schematic.png)

Full-size PDF: [`PCB/production/schematic.pdf`](PCB/production/schematic.pdf)

### Wiring
Every cable that leaves the board. The numbers on the board map match the plug names underneath. Wire colours are only suggestions, so go by pin number (pin 1 is the marked end of each plug).

![Wiring diagram](images/wiring.png)

## What's on the board

**Power**
- 2x XT60 inputs, one per 12 V 12 Ah battery pack
- A 20 A fuse on each pack before they join, so a shorted pack can't get fed by the good one
- 2 screw terminals that send 12 V out to the motor drivers
- A 5 A fuse and an MBR4045CT Schottky in front of everything that isn't a motor
- 5x LM2596 buck modules: 5 V for each ESP32, 5 V just for the servos, and 4.0 V for the SIM800L. Every rail has its own LED so you can see when one dies
- Battery voltage sense on Motion and Sensors (100k/22k dividers)
- 12 V pour on top, ground pour on the bottom, 2 oz copper because up to 20 A comes through here

**Motion ESP32**
- Signal connectors for 2x Cytron MDD10A, which run four 5840-31ZY worm gearmotors
- DRV8825 for the NEMA 17 that lowers the soil probe
- 2 servo headers for the camera pan/tilt
- 2 HC-SR04 connectors, with dividers so the 5 V echo doesn't cook the ESP32

**Sensors ESP32**
- AHT20 + BMP280, BH1750 and MPU6050 on one I2C bus
- NEO-6M GPS
- MAX485 module for the RS485 soil probe, plus a 12 V terminal to power the probe
- Cable to the CYD touchscreen
- 8 spare pins on a header for whatever I forgot

**Audio ESP32**
- 2x MAX98357A amps on one shared I2S bus, gain picked with a jumper on each
- microSD
- SIM800L
- 8 more spare pins

## Stuff I'm not sure about yet

A lot of this board is educated guesses, and one guess already nearly went wrong. The first LM2596 footprint was mirrored, so 12 V would have gone straight into the output side of every buck. I only caught it when I swapped in real 3D models and the module didn't line up with its pads. The pads are now 4 mm squares that fit every clone I could find, but I still want to measure one.

- The probe is the part I'm least sure about. A NEMA 17 on a rack and pinion pushes maybe 20 to 50 N, and the rover can't push down harder than it weighs anyway. Soft or tilled soil should be fine. Dry, packed clay, I don't know yet.
- The 12 V feed to the top motor driver terminal (M2_PWR) goes through about 1.5 mm of copper along the top edge. On 2 oz that's good for maybe 6 or 7 A, which is fine for driving around and probably not fine if both motors on that side stall at once.
- The 5 A fuse covers every buck plus the stepper. My estimate is around 4 A worst case from 12 V, and I haven't measured anything.
- The 5 A stall current per motor is a guess too, and the 20 A fuses are sized off that guess.
- Before I order I still need calipers on the ESP32 row gap (the board assumes 25.4 mm, some 30-pin clones are 22.86 mm), the fuse clip pins, and the pin order on my microSD module.

## If you build one

- Set every LM2596 to its voltage before it's connected to anything, then hot glue the trimmer. 12 V into a MAX98357A kills it instantly, and the SIM800L wants 4.0 V, not 5.
- Set the DRV8825 current limit before you plug the stepper in.
- Use ceramic 20 A fuses, not glass, and zip tie them down. The clips hold by friction and this thing shakes.
- The camera power terminal has ground on pin 1, the opposite of the other power terminals. It's marked − / + on the board, so look before you screw the wires in.
- Put some kapton tape under each buck module. They sit flat on the board and their bottom pins can scratch through the solder mask.

## Files

- `PCB/` - KiCad 10 project (schematic, board, custom footprints, symbols and 3D models). `PCB/production/` has `gerbers.zip` (Gerbers + drill files) and `schematic.pdf`
- `CAD/agribot-pcb.step.zip` - STEP of the assembled board with every module on it, zipped because the raw file is 54 MB and the GitHub website won't take files over 25 MB. The module models are real ones (Espressif, KiCad library, Sector7GP, a SIM800L repo, openbikesensor, and the JLCEDA/EasyEDA Official Library for the fuses and the KF128 terminals), credits are in `PCB/libraries/3dmodels/CREDITS.md`
- `firmware/` - code for the Motion, Sensors and Audio ESP32s
- `BOM.csv` - parts list bought locally in Egypt (Makers Electronics, Micro Ohm, Free Electronics) with links, USD and EGP prices and shipping, about $141 total. The PCB and shipping lines are estimates until I get real quotes
