// AgriBot - SENSORS ESP32 (U11)
#include <Wire.h>
#include "pins_sensors.h"

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial1.begin(115200, SERIAL_8N1, LINK_MOTION_RX, LINK_MOTION_TX);
  Serial2.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  // TODO
}

void loop() {
  // TODO
}
