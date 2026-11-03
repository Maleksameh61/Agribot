// AgriBot - AUDIO ESP32 (U12)
#include <SPI.h>
#include "pins_audio.h"

void setup() {
  Serial.begin(115200);
  pinMode(AMP_SD, OUTPUT); digitalWrite(AMP_SD, LOW);   // amps off until audio starts
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  Serial1.begin(9600, SERIAL_8N1, SIM_RX, SIM_TX);
  Serial2.begin(115200, SERIAL_8N1, LINK_SENSORS_RX, LINK_SENSORS_TX);

  // TODO
}

void loop() {
  // TODO
}
