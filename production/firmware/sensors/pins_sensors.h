// Pin map for the SENSORS ESP32 (U11). Taken from agribot.kicad_sch.
#pragma once

// I2C bus: AHT20 + BMP280 (J9), BH1750 (U4), MPU6050 (U5). 4.7k pull-ups on board
#define I2C_SDA 21
#define I2C_SCL 22

// GPS (NEO-6M, J11)
#define GPS_RX 17   // <- GPS TX
#define GPS_TX 16   // -> GPS RX

// RS485 soil probe (MAX485 auto-direction module, M8)
#define RS485_RX 13 // <- module RO
#define RS485_TX 14 // -> module DI

// Display link (CYD, U1)
#define DISPLAY_RX 26  // <- display TX
#define DISPLAY_TX 27  // -> display RX

// Battery voltage: 100k / 22k divider from the 12 V rail
#define BATT_SENSE   36   // input only
#define BATT_DIVIDER (122.0 / 22.0)

// UART link to the MOTION ESP32
#define LINK_MOTION_RX 25   // <- motion GPIO5
#define LINK_MOTION_TX 4    // -> motion GPIO39

// UART link to the AUDIO ESP32
#define LINK_AUDIO_TX 32    // -> audio GPIO16
#define LINK_AUDIO_RX 15    // <- audio GPIO17

// Spare pins (J16)
#define SPARE1 23
#define SPARE2 19
#define SPARE3 18
#define SPARE4 5
#define SPARE5 39   // input only
#define SPARE6 34   // input only
#define SPARE7 35   // input only
#define SPARE8 33
