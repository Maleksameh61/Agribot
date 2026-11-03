// Pin map for the MOTION ESP32 (U10). Taken from agribot.kicad_sch.
#pragma once

// Motor driver 1 (Cytron MDD10A, J12)
#define MD1_PWM1   26   // 10k pull-down on board
#define MD1_DIR1   25
#define MD1_PWM2   27   // 10k pull-down on board
#define MD1_DIR2   14

// Motor driver 2 (Cytron MDD10A, J13)
#define MD2_PWM1   32   // 10k pull-down on board
#define MD2_DIR1   33
#define MD2_PWM2   19   // 10k pull-down on board
#define MD2_DIR2   18

// Servos
#define SERVO1_PWM 4    // M1
#define SERVO2_PWM 13   // M2

// Stepper driver (DRV8825, U9)
#define STEPPER_STEP  12  // through 10k. GPIO12 is a boot strapping pin: keep it LOW during boot
#define STEPPER_DIR   17
#define STEPPER_EN    23  // active LOW (HIGH = driver off)
#define STEPPER_FAULT 34  // input only, active LOW

// Ultrasonic sensors (echo goes through a 1k/2k divider)
#define US1_TRIG 22   // U2
#define US1_ECHO 21
#define US2_TRIG 15   // U3
#define US2_ECHO 16

// Battery voltage: 100k / 22k divider from the 12 V rail
#define BATT_SENSE     35            // input only
#define BATT_DIVIDER   (122.0 / 22.0)

// UART link to the SENSORS ESP32
#define LINK_SENSORS_TX 5    // -> sensors GPIO25
#define LINK_SENSORS_RX 39   // <- sensors GPIO4 (input only)
