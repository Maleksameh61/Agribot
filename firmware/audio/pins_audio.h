// Pin map for the AUDIO ESP32 (U12). Taken from agribot.kicad_sch.
#pragma once

// I2S to both MAX98357A amps (U6, U7)
#define I2S_BCLK  26
#define I2S_LRCLK 25
#define I2S_DOUT  22
#define AMP_SD    21   // HIGH = amps on

// microSD (J14)
#define SD_SCK  23
#define SD_MISO 18
#define SD_MOSI 19
#define SD_CS   5

// SIM800L (U8)
#define SIM_RX 14   // <- SIM800L TXD
#define SIM_TX 13   // -> SIM800L RXD (through 1k / 3.3k divider)

// UART link to the SENSORS ESP32
#define LINK_SENSORS_RX 16   // <- sensors GPIO32
#define LINK_SENSORS_TX 17   // -> sensors GPIO15

// Spare pins (J15)
#define SPARE1 27
#define SPARE2 33
#define SPARE3 4
#define SPARE4 32
#define SPARE5 35   // input only
#define SPARE6 34   // input only
#define SPARE7 39   // input only
#define SPARE8 36   // input only
