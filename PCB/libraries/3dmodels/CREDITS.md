# 3D model credits

The module models here are gzipped STEP files (`.stpZ`, KiCad opens them directly). Each one is lined up with its footprint, so the offsets in KiCad are all 0. The female sockets on the main board come from the stock KiCad library.

| File | Built from | Source | License |
|---|---|---|---|
| ESP32_DevKit_30pin.stpZ | ESP32-WROOM-32E model on a DOIT-style 30 pin board, plus KiCad library parts (micro USB, CP2102 QFN-28, SOT-223, buttons, LEDs, pin headers) | [espressif/kicad-libraries](https://github.com/espressif/kicad-libraries), [KiCad 3D library](https://github.com/KiCad/kicad-packages3D) | Espressif library license / CC-BY-SA 4.0 with the KiCad library exception |
| LM2596S_Module.stpZ | LM2596S_StepDown_Module (header pins removed, it's flat mounted) | [sector7gp/kikad_stuff](https://github.com/sector7gp/kikad_stuff) | MIT |
| MAX98357A_Module.stpZ | Adafruit_MAX98357A | [sector7gp/kikad_stuff](https://github.com/sector7gp/kikad_stuff) | MIT |
| GY521_MPU6050.stpZ | GY-521_MPU6050 + KiCad 1x08 pin header | [sector7gp/kikad_stuff](https://github.com/sector7gp/kikad_stuff), KiCad 3D library | MIT / CC-BY-SA 4.0 |
| SIM800L_Module.stpZ | sim800l.step + KiCad 1x06 pin headers | [shridattdudhat/SIM800L-Core-Module-KiCad-Footprint](https://github.com/shridattdudhat/SIM800L-Core-Module-KiCad-Footprint), KiCad 3D library | GPL-3.0 / CC-BY-SA 4.0 |
| MicroSD_Module.stpZ | TF Micro SD Card Module (recoloured, stood upright) | [openbikesensor/OpenBikeSensor_PCB_ext3DModels](https://github.com/openbikesensor/OpenBikeSensor_PCB_ext3DModels) | see source repo |
| DRV8825_Carrier.stpZ | Pololu-size carrier I drew, populated with KiCad library parts (HTSSOP-28, Bourns 3314J trimmer, 0805/0603 parts, pin headers) | KiCad 3D library | CC-BY-SA 4.0 with the KiCad library exception |
