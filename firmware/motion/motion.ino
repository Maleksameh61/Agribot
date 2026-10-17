// AgriBot - MOTION ESP32 (U10)
#include "pins_motion.h"

void setup() {
  Serial.begin(115200);

  // safe states at power-up
  pinMode(STEPPER_EN, OUTPUT);   digitalWrite(STEPPER_EN, HIGH);   // stepper off
  pinMode(STEPPER_STEP, OUTPUT); digitalWrite(STEPPER_STEP, LOW);
  pinMode(STEPPER_DIR, OUTPUT);
  pinMode(STEPPER_FAULT, INPUT);

  const int motorPins[] = {MD1_PWM1, MD1_DIR1, MD1_PWM2, MD1_DIR2, MD2_PWM1, MD2_DIR1, MD2_PWM2, MD2_DIR2};
  for (int p : motorPins) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }   // motors stopped

  pinMode(US1_TRIG, OUTPUT); pinMode(US1_ECHO, INPUT);
  pinMode(US2_TRIG, OUTPUT); pinMode(US2_ECHO, INPUT);

  Serial1.begin(115200, SERIAL_8N1, LINK_SENSORS_RX, LINK_SENSORS_TX);

  // TODO
}

void loop() {
  // TODO
}
