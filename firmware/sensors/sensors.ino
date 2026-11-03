// AgriBot - SENSORS ESP32 (the main one)
//
// Everything goes through this chip:
//   - puts up the "AgriBot" WiFi on channel 6, serves the phone page,
//     WebSocket for driving and live data, UDP port if I ever make an app
//   - missions: drives to GPS points, gets heading from the gyro + GPS,
//     backs off obstacles, gives up if it's stuck / tipped / battery dead,
//     does the soil probe at each point
//   - reads the GPS, AHT20, BMP280, BH1750, MPU6050 and the RS485 soil probe
//   - saves every soil reading with where it was taken to /log.csv
//   - tells Motion how to drive (20 times a second), tells Audio what to
//     play or text, tells the CYD what to show
//
// UARTs: this chip needs 5 serial links (Motion, Audio, GPS, soil probe,
// CYD) and only has 2 free hardware UARTs, because UART0 is the USB port.
// Motion and GPS get the real ones. Audio, CYD and the probe are on
// EspSoftwareSerial. They're slow and every frame has a CRC, so if WiFi
// messes up a bit I lose one frame, not get a wrong number.
// (I didn't notice this until the board was done. Next rev the CYD goes on WiFi.)
//
// Board: ESP32 Dev Module, ESP32 core 3.x, default partition scheme
// (LittleFS uses the spiffs part for the logs). It's at 89% of flash already.
//
// Libraries: ESPAsyncWebServer + AsyncTCP (the ESP32Async ones), ArduinoJson 7,
// TinyGPSPlus, EspSoftwareSerial, Adafruit BMP280

#include <initializer_list>
#include <ArduinoJson.h>   // has to be before ESPAsyncWebServer.h or its JSON stuff turns off (took me a while)
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <AsyncUDP.h>
#include <LittleFS.h>
#include <Wire.h>
#include <TinyGPSPlus.h>
#include <SoftwareSerial.h>
#include <Adafruit_BMP280.h>
#include "pins_sensors.h"
#include "agrilink.h"
#include "web_page.h"

// ======================= CONFIG =======================
#define AP_SSID        "AgriBot"
#define AP_PASS        "agribot2026"   // has to be 8+ characters
#define AP_CHANNEL     6
#define UDP_DRIVE_PORT 4210            // for an app later, see udpPacket()

#define AUDIO_LINK_BAUD   38400        // must match LINK_BAUD in audio.ino
#define DISPLAY_LINK_BAUD 38400        // must match LINK_BAUD in display.ino

// ZTS-3002 soil probe, Modbus RTU. out of the box it's address 1, 4800 8N1.
// registers 0-6: moisture x0.1 %, temp x0.1 C (signed), EC uS/cm, pH x0.1, N, P, K
#define SOIL_BAUD   4800
#define SOIL_ADDR   1
#define SOIL_TRIES  3

#define PROBE_DEPTH_MM   120     // 12 cm into the ground
#define PROBE_SETTLE_MS  8000    // the numbers jump around for a few seconds after it goes in
#define PROBE_MOVE_TIMEOUT_MS 40000

// Navigation
#define ARRIVE_RADIUS_M   2.5f   // NEO-6M is about 2.5 m on a good day, tighter than this and it circles forever
#define CRUISE_THROTTLE   1000   // it's 1.4 km/h flat out, no reason to go slower
#define APPROACH_THROTTLE 700    // last 5 m
#define STEER_KP          12.0f  // how hard it steers per degree it's off
#define STEER_MAX         450
#define PIVOT_ABOVE_DEG   40.0f  // more than this off -> stop and turn in place
#define PIVOT_DONE_DEG    10.0f
#define PIVOT_STEER       700
#define HEADING_FIX_M     4.0f   // how far it has to drive straight before I trust the GPS direction
#define GPS_MIN_SATS      5
#define GPS_MAX_HDOP      3.0f
#define GPS_LOST_ABORT_MS 60000
#define STUCK_WINDOW_MS   30000  // driving forward this long...
#define STUCK_MIN_M       3.0f   // ...but moved less than this = stuck (should have done ~11 m)
#define TILT_LIMIT_DEG    25.0f
#define MAX_AVOIDS_PER_WP 3

// MPU6050 is flat with Z up. turn right in place and the heading on the phone
// has to go UP. if it goes down, change this to +1
#define YAW_SIGN -1.0f

#define PHONE_TIMEOUT_MS 300     // phone goes quiet -> stop. Motion has its own 150 ms on top of this

#define BATT_LOW_MV  11600       // 12 V lead acid while driving
#define BATT_CRIT_MV 11000       // give up on the mission and text me
#define SMS_ALERTS   1           // text me when it finishes, gets stuck, gives up or the battery's low

#define MAX_WP 50

// ======================= links =======================
EspSoftwareSerial::UART swAudio, swDisplay, swSoil;
AgriLink motionLink(Serial1);
AgriLink audioLink(swAudio);
AgriLink displayLink(swDisplay);
SemaphoreHandle_t motionTxLock;
TinyGPSPlus gps;

// ======================= state =======================
uint8_t mode = MODE_IDLE;

MotionState motion = {};
AudioState audio = {};

// what Motion should be doing right now, the TX task sends it every 50 ms
portMUX_TYPE cmdMux = portMUX_INITIALIZER_UNLOCKED;
DriveCmd outCmd = {0, 0, 90, 90, 0};
volatile uint32_t loopBeatMs = 0;

// latest stick position from the phone (written from the WebSocket task)
portMUX_TYPE phoneMux = portMUX_INITIALIZER_UNLOCKED;
struct { int16_t t, s; uint8_t pan, tilt; uint32_t ms; } phone = {0, 0, 90, 90, 0};
volatile uint32_t phoneSeenMs = 0;

// buttons from the phone / CYD / SMS, loop() handles them
enum : uint8_t {
  C_STOP = 1, C_MANUAL, C_START, C_PAUSE, C_PROBE, C_RAISE, C_HOME, C_HORN, C_WP_HERE, C_WP_CLEAR,
};
struct Command { uint8_t code; int32_t arg; };
QueueHandle_t cmdQueue;

// sensors
float airTemp = NAN, humidity = NAN, pressure = NAN, lux = NAN;
float roll = 0, pitch = 0, yawDeg = 0;
float gyroBias[3] = {0, 0, 0};
float gzLast = 0;
bool bmpOk = false, ahtOk = false, bhOk = false, mpuOk = false;
float battmV = 0;
Adafruit_BMP280 bmp;

// heading = gyro yaw + an offset I fix from GPS every time it drives straight
float headingOffset = 0;
bool headingValid = false;
double anchorLat = 0, anchorLon = 0; float anchorYaw = 0; bool anchorSet = false;

// soil
struct SoilReading { bool valid; float moisture, temp, ph; uint16_t ec, n, p, k; double lat, lon; char at[20]; };
SoilReading lastSoil = {};

// camera. the CAM asks /api/cam what mode to be in and sends back what it saw
bool camScan = false;
char camLabel[40] = "";
uint32_t camSeenMs = 0;

// mission
struct Waypoint { double lat, lon; uint8_t probe; };
Waypoint wps[MAX_WP];
uint8_t wpCount = 0, wpIndex = 0;
Waypoint stagedWps[MAX_WP]; volatile int stagedCount = -1;   // from POST /api/mission
portMUX_TYPE wpMux = portMUX_INITIALIZER_UNLOCKED;

enum MissionStep : uint8_t {
  MS_IDLE, MS_WAIT_GPS, MS_ACQUIRE, MS_TURN, MS_DRIVE, MS_AVOID_BACK, MS_AVOID_TURN, MS_AVOID_FWD,
  MS_PROBING, MS_PAUSED, MS_DONE, MS_ABORTED,
};
const char* const STEP_NAMES[] = {"idle", "wait_gps", "acquire", "turn", "drive", "avoid", "avoid", "avoid",
                                  "probing", "paused", "done", "aborted"};
uint8_t step = MS_IDLE;
uint32_t stepSince = 0;
uint8_t avoidCount = 0, acquireTries = 0;
float avoidStartYaw = 0;
uint32_t gpsLostSince = 0;
double stuckRefLat = 0, stuckRefLon = 0; uint32_t stuckRefMs = 0; bool stuckArmed = false;
uint16_t readingsThisMission = 0;
bool raisingBeforeStart = false;   // mission (re)started with the probe left down

// probe job, missions use it and so does the "probe now" button
enum ProbeJob : uint8_t { PJ_NONE, PJ_STOPPING, PJ_LOWER, PJ_SETTLE, PJ_READ, PJ_RAISE, PJ_OK, PJ_FAILED };
uint8_t probeJob = PJ_NONE;
uint32_t probeJobSince = 0, probeLastSend = 0;
uint8_t soilTries = 0;
bool probeReadOk = false;
uint32_t lastHomeSent = 0;

char statusMsg[60] = "booting";
bool lowBattWarned = false;

// ======================= small helpers =======================
static float wrap180(float a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
static float wrap360(float a) { while (a >= 360) a -= 360; while (a < 0) a += 360; return a; }
static float heading() { return wrap360(yawDeg + headingOffset); }

static void setStatus(const char* fmt, ...) {
  char buf[60];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  if (strcmp(buf, statusMsg)) {
    strlcpy(statusMsg, buf, sizeof statusMsg);
    Serial.printf("[status] %s\n", statusMsg);
    displayLink.sendText(MSG_STATUS_TEXT, statusMsg);
  }
}

static void motionSend(uint8_t type, const void* p, uint8_t len) {
  xSemaphoreTake(motionTxLock, portMAX_DELAY);
  motionLink.send(type, p, len);
  xSemaphoreGive(motionTxLock);
}

static void setDrive(int16_t throttle, int16_t steer, uint8_t flags = 0) {
  portENTER_CRITICAL(&cmdMux);
  outCmd.throttle = throttle; outCmd.steer = steer; outCmd.flags = flags;
  portEXIT_CRITICAL(&cmdMux);
}
static void setCamera(uint8_t pan, uint8_t tilt) {
  portENTER_CRITICAL(&cmdMux);
  outCmd.pan = pan; outCmd.tilt = tilt;
  portEXIT_CRITICAL(&cmdMux);
}
static int16_t currentThrottle() { portENTER_CRITICAL(&cmdMux); int16_t t = outCmd.throttle; portEXIT_CRITICAL(&cmdMux); return t; }

static void probeSend(uint8_t c, uint16_t depth = 0) {
  ProbeCmd p = {c, depth};
  motionSend(MSG_PROBE, &p, sizeof p);
}

static void play(uint8_t clip) { PlayCmd p = {clip, 255}; audioLink.send(MSG_PLAY, p); }

static void sms(const char* fmt, ...) {
#if SMS_ALERTS
  char buf[60];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  audioLink.sendText(MSG_SMS_SEND, buf);
#endif
}

static bool gpsGood() {
  return gps.location.isValid() && gps.location.age() < 3000 && gps.satellites.value() >= GPS_MIN_SATS &&
         gps.hdop.isValid() && gps.hdop.hdop() <= GPS_MAX_HDOP;
}

static void pushCommand(uint8_t code, int32_t arg = 0) { Command c = {code, arg}; xQueueSend(cmdQueue, &c, 0); }

// ======================= motion TX task =======================
// sends the drive command every 50 ms even if loop() is busy. if loop() hasn't
// run for 200 ms it sends 0 instead, so if this chip freezes Motion doesn't
// keep doing whatever the last command was (like full forward)
static void motionTxTask(void*) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    DriveCmd c;
    portENTER_CRITICAL(&cmdMux); c = outCmd; portEXIT_CRITICAL(&cmdMux);
    if (millis() - loopBeatMs > 200) { c.throttle = 0; c.steer = 0; }
    motionSend(MSG_DRIVE, &c, sizeof c);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(50));
  }
}

// ======================= I2C sensors =======================
static bool i2cWrite(uint8_t addr, std::initializer_list<uint8_t> bytes) {
  Wire.beginTransmission(addr);
  for (uint8_t b : bytes) Wire.write(b);
  return Wire.endTransmission() == 0;
}
static bool i2cRead(uint8_t addr, uint8_t* buf, uint8_t n) {
  if (Wire.requestFrom(addr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

#define AHT_ADDR 0x38
#define BH_ADDR  0x23   // ADDR tied to GND
#define MPU_ADDR 0x68

static void sensorsBegin() {
  Wire.begin(I2C_SDA, I2C_SCL, 400000);

  // AHT20: calibrate if the status says it isn't
  uint8_t s;
  if (i2cWrite(AHT_ADDR, {0x71}) && i2cRead(AHT_ADDR, &s, 1)) {
    if (!(s & 0x08)) { i2cWrite(AHT_ADDR, {0xBE, 0x08, 0x00}); delay(10); }
    ahtOk = true;
  }
  bmpOk = bmp.begin(0x77) || bmp.begin(0x76);   // mine should be 0x77 but some boards are 0x76
  if (bmpOk) bmp.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2,
                             Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16,
                             Adafruit_BMP280::STANDBY_MS_500);
  bhOk = i2cWrite(BH_ADDR, {0x01}) && i2cWrite(BH_ADDR, {0x10});   // on, continuous high res

  // MPU6050: wake, +-250 dps, +-2 g, 44 Hz low-pass
  mpuOk = i2cWrite(MPU_ADDR, {0x6B, 0x00});
  if (mpuOk) {
    i2cWrite(MPU_ADDR, {0x1A, 0x03});
    i2cWrite(MPU_ADDR, {0x1B, 0x00});
    i2cWrite(MPU_ADDR, {0x1C, 0x00});
    delay(100);
    // gyro offset, so don't touch the rover for this second
    float sum[3] = {0, 0, 0}; int n = 0;
    for (int i = 0; i < 200; i++) {
      uint8_t b[6];
      if (i2cWrite(MPU_ADDR, {0x43}) && i2cRead(MPU_ADDR, b, 6)) {
        for (int k = 0; k < 3; k++) sum[k] += (int16_t)((b[2 * k] << 8) | b[2 * k + 1]) / 131.0f;
        n++;
      }
      delay(5);
    }
    if (n) for (int k = 0; k < 3; k++) gyroBias[k] = sum[k] / n;
  }
  Serial.printf("[i2c] AHT20 %s  BMP280 %s  BH1750 %s  MPU6050 %s\n", ahtOk ? "ok" : "MISSING",
                bmpOk ? "ok" : "MISSING", bhOk ? "ok" : "MISSING", mpuOk ? "ok" : "MISSING");
}

static void imuService() {
  static uint32_t last = 0;
  uint32_t now = micros();
  if (!mpuOk || now - last < 10000) return;   // 100 Hz
  float dt = (now - last) / 1e6f;
  if (dt > 0.1f) dt = 0.01f;
  last = now;
  uint8_t b[14];
  if (!i2cWrite(MPU_ADDR, {0x3B}) || !i2cRead(MPU_ADDR, b, 14)) return;
  auto v = [&](int i) { return (int16_t)((b[i] << 8) | b[i + 1]); };
  float ax = v(0) / 16384.0f, ay = v(2) / 16384.0f, az = v(4) / 16384.0f;
  float gx = v(8) / 131.0f - gyroBias[0], gy = v(10) / 131.0f - gyroBias[1], gz = v(12) / 131.0f - gyroBias[2];
  float accRoll = atan2f(ay, az) * RAD_TO_DEG;
  float accPitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * RAD_TO_DEG;
  roll = 0.98f * (roll + gx * dt) + 0.02f * accRoll;
  pitch = 0.98f * (pitch + gy * dt) + 0.02f * accPitch;
  if (fabsf(gz) > 0.3f) yawDeg += YAW_SIGN * gz * dt;   // ignore tiny values so it doesn't drift while parked
  gzLast = gz;
}

static void envService() {
  static uint32_t last = 0;
  static bool ahtPending = false;
  uint32_t now = millis();
  if (ahtPending && now - last >= 90) {   // AHT20 takes ~80 ms to measure
    ahtPending = false;
    uint8_t d[7];
    if (i2cRead(AHT_ADDR, d, 7) && !(d[0] & 0x80)) {
      uint32_t rh = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
      uint32_t tc = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
      humidity = rh * 100.0f / 1048576.0f;
      airTemp = tc * 200.0f / 1048576.0f - 50.0f;
    }
  }
  if (now - last < 2000) return;
  last = now;
  if (ahtOk) { i2cWrite(AHT_ADDR, {0xAC, 0x33, 0x00}); ahtPending = true; }
  if (bmpOk) pressure = bmp.readPressure() / 100.0f;
  if (bhOk) { uint8_t d[2]; if (i2cRead(BH_ADDR, d, 2)) lux = ((d[0] << 8) | d[1]) / 1.2f; }
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(BATT_SENSE);
  battmV = mv / 8.0f * BATT_DIVIDER;
}

// ======================= heading from GPS =======================
static void headingService() {
  // new fix? can't use isUpdated() because every lat() call anywhere clears it
  static uint32_t lastFixes = 0;
  if (gps.sentencesWithFix() == lastFixes) return;
  lastFixes = gps.sentencesWithFix();
  double lat = gps.location.lat(), lon = gps.location.lng();
  // only learn while it's actually going straight
  bool straight = motion.left_out > 300 && motion.right_out > 300 && abs(motion.left_out - motion.right_out) < 120;
  if (!straight || !gpsGood()) { anchorSet = false; return; }
  if (!anchorSet) { anchorLat = lat; anchorLon = lon; anchorYaw = yawDeg; anchorSet = true; return; }
  if (fabsf(yawDeg - anchorYaw) > 8) { anchorLat = lat; anchorLon = lon; anchorYaw = yawDeg; return; }  // it curved, start over
  double d = TinyGPSPlus::distanceBetween(anchorLat, anchorLon, lat, lon);
  if (d < HEADING_FIX_M) return;
  float bearing = TinyGPSPlus::courseTo(anchorLat, anchorLon, lat, lon);
  float newOffset = wrap180(bearing - (anchorYaw + yawDeg) / 2);
  if (!headingValid) { headingOffset = newOffset; headingValid = true; Serial.printf("[hdg] first fix %.0f\n", heading()); }
  else headingOffset = wrap180(headingOffset + 0.5f * wrap180(newOffset - headingOffset));
  anchorLat = lat; anchorLon = lon; anchorYaw = yawDeg;
}

// ======================= RS485 soil probe (Modbus RTU) =======================
static uint16_t modbusCrc(const uint8_t* d, int n) {
  uint16_t crc = 0xFFFF;
  while (n--) { crc ^= *d++; for (int i = 0; i < 8; i++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1; }
  return crc;
}

uint8_t soilBuf[32]; uint8_t soilLen = 0; uint32_t soilAskedMs = 0; bool soilWaiting = false;

static void soilAsk() {
  uint8_t q[8] = {SOIL_ADDR, 0x03, 0x00, 0x00, 0x00, 0x07, 0, 0};
  uint16_t c = modbusCrc(q, 6); q[6] = c & 0xFF; q[7] = c >> 8;
  while (swSoil.available()) swSoil.read();
  swSoil.write(q, 8);
  soilLen = 0; soilAskedMs = millis(); soilWaiting = true;
}

// returns 1 = got a reading, -1 = failed/timed out, 0 = still waiting
static int soilPoll() {
  if (!soilWaiting) return -1;
  while (swSoil.available() && soilLen < sizeof soilBuf) soilBuf[soilLen++] = swSoil.read();
  // look for "addr 03 0E" in case there's junk before it
  for (int i = 0; i + 19 <= soilLen; i++) {
    uint8_t* r = soilBuf + i;
    if (r[0] != SOIL_ADDR || r[1] != 0x03 || r[2] != 14) continue;
    uint16_t c = modbusCrc(r, 17);
    if ((r[17] | (r[18] << 8)) != c) continue;
    auto reg = [&](int k) { return (uint16_t)((r[3 + 2 * k] << 8) | r[4 + 2 * k]); };
    lastSoil.moisture = reg(0) / 10.0f;
    lastSoil.temp = (int16_t)reg(1) / 10.0f;
    lastSoil.ec = reg(2);
    lastSoil.ph = reg(3) / 10.0f;
    lastSoil.n = reg(4); lastSoil.p = reg(5); lastSoil.k = reg(6);
    lastSoil.lat = gps.location.isValid() ? gps.location.lat() : 0;
    lastSoil.lon = gps.location.isValid() ? gps.location.lng() : 0;
    if (gps.date.isValid() && gps.time.isValid())
      snprintf(lastSoil.at, sizeof lastSoil.at, "%04u-%02u-%02u %02u:%02u", gps.date.year(), gps.date.month(),
               gps.date.day(), gps.time.hour(), gps.time.minute());
    else snprintf(lastSoil.at, sizeof lastSoil.at, "t+%lus", millis() / 1000);
    lastSoil.valid = true;
    soilWaiting = false;
    return 1;
  }
  if (millis() - soilAskedMs > 700) { soilWaiting = false; return -1; }
  return 0;
}

static void logReading() {
  bool fresh = !LittleFS.exists("/log.csv");
  File f = LittleFS.open("/log.csv", FILE_APPEND);
  if (!f) { Serial.println("[log] can't open /log.csv"); return; }
  if (fresh) f.println("time_utc,lat,lon,wp,moisture_pct,soil_temp_c,ec_uScm,ph,n_mgkg,p_mgkg,k_mgkg,air_temp_c,humidity_pct,pressure_hpa,lux,camera");
  f.printf("%s,%.7f,%.7f,%d,%.1f,%.1f,%u,%.1f,%u,%u,%u,%.1f,%.0f,%.1f,%.0f,%s\n", lastSoil.at, lastSoil.lat, lastSoil.lon,
           mode == MODE_MISSION ? wpIndex + 1 : 0, lastSoil.moisture, lastSoil.temp, lastSoil.ec, lastSoil.ph,
           lastSoil.n, lastSoil.p, lastSoil.k, airTemp, humidity, pressure, lux, camLabel);
  f.close();
}

// ======================= probe job =======================
static void probeStart() {
  probeJob = PJ_STOPPING; probeJobSince = millis(); probeLastSend = 0;
  setDrive(0, 0);
  setStatus("stopping to probe");
}

static void probeService() {
  uint32_t now = millis();
  uint32_t inStep = now - probeJobSince;
  auto go = [&](uint8_t s) { probeJob = s; probeJobSince = now; probeLastSend = 0; };

  switch (probeJob) {
    case PJ_STOPPING:
      setDrive(0, 0);
      if (inStep > 600 && motion.left_out == 0 && motion.right_out == 0) { go(PJ_LOWER); play(CLIP_PROBE_DOWN); }
      break;
    case PJ_LOWER:
      setStatus("probe going down");
      if (motion.probe_state == PROBE_DOWN) { go(PJ_SETTLE); break; }
      if (motion.probe_state == PROBE_FAULT || inStep > PROBE_MOVE_TIMEOUT_MS) { go(PJ_RAISE); probeReadOk = false; setStatus("probe failed to go down"); break; }
      if (motion.probe_state != PROBE_LOWERING && now - probeLastSend > 1000) { probeLastSend = now; probeSend(PROBE_CMD_LOWER, PROBE_DEPTH_MM); }
      break;
    case PJ_SETTLE:
      camScan = true;   // let the CAM look at the plants while we wait
      setStatus("probe in, settling");
      if (inStep > PROBE_SETTLE_MS) { go(PJ_READ); soilTries = 0; soilAsk(); }
      break;
    case PJ_READ: {
      int r = soilPoll();
      if (r == 1) {
        probeReadOk = true;
        logReading();
        readingsThisMission++;
        play(CLIP_READING_SAVED);
        setStatus("read: %.0f%% EC%u pH%.1f", lastSoil.moisture, lastSoil.ec, lastSoil.ph);
        go(PJ_RAISE);
      } else if (r == -1) {
        if (++soilTries < SOIL_TRIES) soilAsk();
        else { probeReadOk = false; setStatus("soil probe not answering"); go(PJ_RAISE); }
      }
      break;
    }
    case PJ_RAISE:
      camScan = false;
      if (motion.probe_state == PROBE_UP) { go(probeReadOk ? PJ_OK : PJ_FAILED); break; }
      if (inStep > PROBE_MOVE_TIMEOUT_MS) {
        go(PJ_FAILED);
        setStatus("PROBE STUCK DOWN");
        play(CLIP_ERROR);
        sms("AgriBot: probe stuck down, wheels locked");
        break;
      }
      if (motion.probe_state != PROBE_RAISING && now - probeLastSend > 1000) { probeLastSend = now; probeSend(PROBE_CMD_RAISE); }
      break;
    default: break;
  }
  // "probe now" from the phone or CYD is done, clear it
  if (mode != MODE_MISSION && (probeJob == PJ_OK || probeJob == PJ_FAILED)) probeJob = PJ_NONE;
}

// ======================= mission =======================
static void stepTo(uint8_t s) {
  if (s == step) return;
  step = s; stepSince = millis();
  stuckArmed = false;
}

static void saveMission() {
  File f = LittleFS.open("/mission.bin", FILE_WRITE);
  if (!f) return;
  f.write(&wpCount, 1);
  f.write((uint8_t*)wps, sizeof(Waypoint) * wpCount);
  f.close();
}
static void loadMission() {
  File f = LittleFS.open("/mission.bin");
  if (!f) return;
  uint8_t n = 0;
  if (f.read(&n, 1) == 1 && n <= MAX_WP && f.read((uint8_t*)wps, sizeof(Waypoint) * n) == (int)(sizeof(Waypoint) * n)) wpCount = n;
  f.close();
  Serial.printf("[mission] loaded %u waypoints\n", wpCount);
}

static void missionAbort(const char* why) {
  setDrive(0, 0);
  stepTo(MS_ABORTED);
  mode = MODE_IDLE;
  setStatus("ABORT: %s", why);
  play(CLIP_ERROR);
  sms("AgriBot ABORT wp%u/%u: %s %.6f,%.6f", wpIndex + 1, wpCount, why, gps.location.lat(), gps.location.lng());
}

static void nextWaypoint() {
  wpIndex++;
  avoidCount = 0;
  if (wpIndex >= wpCount) {
    setDrive(0, 0);
    stepTo(MS_DONE);
    mode = MODE_IDLE;
    setStatus("mission done, %u readings", readingsThisMission);
    play(CLIP_MISSION_DONE);
    sms("AgriBot: mission done, %u readings saved", readingsThisMission);
  } else {
    stepTo(MS_WAIT_GPS);
  }
}

static void missionStart() {
  if (!wpCount) { setStatus("no waypoints"); return; }
  if (step == MS_PAUSED && wpIndex < wpCount) setStatus("resuming at wp %u", wpIndex + 1);
  else { wpIndex = 0; readingsThisMission = 0; setStatus("mission start, %u points", wpCount); }
  mode = MODE_MISSION;
  avoidCount = 0; acquireTries = 0;
  probeJob = PJ_NONE;
  play(CLIP_MISSION_START);
  // if the probe got left down, raise it first
  if (motion.probe_state == PROBE_DOWN) {
    probeJob = PJ_RAISE; probeJobSince = millis(); probeReadOk = false; raisingBeforeStart = true; stepTo(MS_PROBING);
  }
  else stepTo(MS_WAIT_GPS);
}

static void missionService() {
  if (mode != MODE_MISSION) return;
  uint32_t now = millis();
  uint32_t inStep = now - stepSince;
  bool navigating = (step == MS_ACQUIRE || step == MS_TURN || step == MS_DRIVE || (step >= MS_AVOID_BACK && step <= MS_AVOID_FWD));

  // ---- guards ----
  if (fabsf(roll) > TILT_LIMIT_DEG || fabsf(pitch) > TILT_LIMIT_DEG) { missionAbort("tilted"); return; }
  if (battmV > 5000 && battmV < BATT_CRIT_MV) { missionAbort("battery low"); return; }
  if (!motionLink.alive(500)) { setDrive(0, 0); setStatus("no link to Motion"); return; }
  if (navigating && !gpsGood()) {
    setDrive(0, 0);
    if (!gpsLostSince) gpsLostSince = now;
    setStatus("waiting for GPS");
    if (now - gpsLostSince > GPS_LOST_ABORT_MS) missionAbort("GPS lost");
    return;
  }
  gpsLostSince = 0;

  // stuck = driving forward for 30 s but barely moved
  if (navigating && currentThrottle() > 0 && gps.location.isValid()) {
    double lat = gps.location.lat(), lon = gps.location.lng();
    if (!stuckArmed) { stuckRefLat = lat; stuckRefLon = lon; stuckRefMs = now; stuckArmed = true; }
    else if (TinyGPSPlus::distanceBetween(stuckRefLat, stuckRefLon, lat, lon) > STUCK_MIN_M) { stuckRefLat = lat; stuckRefLon = lon; stuckRefMs = now; }
    else if (now - stuckRefMs > STUCK_WINDOW_MS) { missionAbort("stuck"); return; }
  } else if (currentThrottle() <= 0) stuckArmed = false;

  // Motion says something's in front
  if ((step == MS_DRIVE || step == MS_ACQUIRE || step == MS_AVOID_FWD) && (motion.flags & MOTION_FLAG_OBSTACLE)) {
    if (++avoidCount > MAX_AVOIDS_PER_WP) { setStatus("blocked, skipping wp %u", wpIndex + 1); play(CLIP_ERROR); nextWaypoint(); return; }
    play(CLIP_OBSTACLE);
    setStatus("obstacle, backing off (%u)", avoidCount);
    stepTo(MS_AVOID_BACK);
  }

  const Waypoint& wp = wps[wpIndex];
  double dist = gps.location.isValid() ? TinyGPSPlus::distanceBetween(gps.location.lat(), gps.location.lng(), wp.lat, wp.lon) : 1e9;
  float bearing = gps.location.isValid() ? TinyGPSPlus::courseTo(gps.location.lat(), gps.location.lng(), wp.lat, wp.lon) : 0;
  float err = wrap180(bearing - heading());

  switch (step) {
    case MS_WAIT_GPS:
      setDrive(0, 0);
      if (gpsGood()) {
        if (dist < ARRIVE_RADIUS_M) { stepTo(MS_DRIVE); break; }   // already there lol
        stepTo(headingValid ? MS_TURN : MS_ACQUIRE);
      }
      break;

    case MS_ACQUIRE:   // drive straight until the GPS tells us which way we're facing
      setStatus("finding heading");
      setDrive(800, 0);
      if (headingValid) { stepTo(MS_TURN); break; }
      if (inStep > 15000) {
        if (++acquireTries >= 3) missionAbort("no heading");
        else stepTo(MS_ACQUIRE), stepSince = now;
      }
      break;

    case MS_TURN:
      setStatus("turning to wp %u (%.0f deg)", wpIndex + 1, err);
      if (fabsf(err) < PIVOT_DONE_DEG || inStep > 15000) { stepTo(MS_DRIVE); break; }
      setDrive(0, err > 0 ? PIVOT_STEER : -PIVOT_STEER);
      break;

    case MS_DRIVE: {
      if (dist < ARRIVE_RADIUS_M) {
        setDrive(0, 0);
        play(CLIP_WAYPOINT);
        setStatus("at wp %u", wpIndex + 1);
        if (wp.probe) { probeStart(); stepTo(MS_PROBING); }
        else nextWaypoint();
        break;
      }
      if (fabsf(err) > PIVOT_ABOVE_DEG) { stepTo(MS_TURN); break; }
      int16_t thr = dist < 5 ? APPROACH_THROTTLE : CRUISE_THROTTLE;
      int16_t st = (int16_t)constrain(STEER_KP * err, -STEER_MAX, STEER_MAX);
      setDrive(thr, st);
      setStatus("wp %u/%u  %.1f m", wpIndex + 1, wpCount, dist);
      break;
    }

    case MS_AVOID_BACK:
      setDrive(-700, 0, DRIVE_FLAG_IGNORE_OBSTACLE);
      if (inStep > 2000) { avoidStartYaw = yawDeg; stepTo(MS_AVOID_TURN); }
      break;
    case MS_AVOID_TURN:
      setDrive(0, PIVOT_STEER);
      if (fabsf(yawDeg - avoidStartYaw) > 45 || inStep > 6000) stepTo(MS_AVOID_FWD);
      break;
    case MS_AVOID_FWD:
      setDrive(800, 0);
      if (inStep > 4000) stepTo(headingValid ? MS_TURN : MS_ACQUIRE);
      break;

    case MS_PROBING:
      if (probeJob == PJ_OK || probeJob == PJ_FAILED || probeJob == PJ_NONE) {
        bool stuckDown = (probeJob == PJ_FAILED && motion.probe_state != PROBE_UP);
        probeJob = PJ_NONE;
        if (stuckDown) { missionAbort("probe stuck"); break; }
        if (raisingBeforeStart) { raisingBeforeStart = false; stepTo(MS_WAIT_GPS); break; }
        nextWaypoint();
      }
      break;
    default: break;
  }
}

// ======================= commands =======================
static void handleCommand(const Command& c) {
  switch (c.code) {
    case C_STOP:
      setDrive(0, 0);
      if (mode == MODE_MISSION) { stepTo(MS_PAUSED); setStatus("paused at wp %u", wpIndex + 1); }
      else setStatus("stopped");
      mode = MODE_IDLE;
      if (probeJob != PJ_NONE && probeJob != PJ_OK && probeJob != PJ_FAILED) { probeSend(PROBE_CMD_STOP); probeJob = PJ_NONE; camScan = false; }
      audioLink.send(MSG_STOP_AUDIO, nullptr, 0);
      break;
    case C_PAUSE:
      if (mode == MODE_MISSION) { setDrive(0, 0); mode = MODE_IDLE; stepTo(MS_PAUSED); setStatus("paused at wp %u", wpIndex + 1); }
      break;
    case C_MANUAL:
      if (mode == MODE_MISSION) { stepTo(MS_PAUSED); }
      mode = MODE_MANUAL; setStatus("manual");
      break;
    case C_START: missionStart(); break;
    case C_PROBE:
      if (mode == MODE_MISSION) break;
      mode = MODE_IDLE; probeStart();
      break;
    case C_RAISE:
      if (mode == MODE_MISSION) break;
      probeJob = PJ_NONE; probeSend(PROBE_CMD_RAISE); setStatus("raising probe");
      break;
    case C_HOME:
      if (mode == MODE_MISSION) break;
      probeJob = PJ_NONE; probeSend(PROBE_CMD_HOME); lastHomeSent = millis(); setStatus("homing probe");
      break;
    case C_HORN: play(CLIP_HORN); break;
    case C_WP_HERE:
      if (mode == MODE_MISSION) { setStatus("pause the mission first"); break; }
      if (!gpsGood()) { setStatus("no good GPS fix yet"); break; }
      if (wpCount >= MAX_WP) { setStatus("waypoint list full"); break; }
      wps[wpCount++] = {gps.location.lat(), gps.location.lng(), (uint8_t)(c.arg ? 1 : 0)};
      saveMission();
      setStatus("added wp %u", wpCount);
      break;
    case C_WP_CLEAR:
      if (mode == MODE_MISSION) { setStatus("pause the mission first"); break; }
      wpCount = 0; wpIndex = 0; stepTo(MS_IDLE); saveMission(); setStatus("waypoints cleared");
      break;
  }
}

static void smsCommand(const char* raw) {
  char t[16]; int n = 0;
  for (; *raw && n < 15; raw++) if (!isspace(*raw)) t[n++] = toupper(*raw);
  t[n] = 0;
  if (!strcmp(t, "STOP")) { pushCommand(C_STOP); sms("AgriBot: stopped"); }
  else if (!strcmp(t, "GO") || !strcmp(t, "START")) pushCommand(C_START);
  else if (!strcmp(t, "STATUS") || !strcmp(t, "WHERE"))
    sms("%.1fV %s wp%u/%u %.6f,%.6f", battmV / 1000, STEP_NAMES[step], min<int>(wpIndex + 1, wpCount), wpCount,
        gps.location.lat(), gps.location.lng());
  else sms("AgriBot: send STATUS, GO or STOP");
}

// ======================= manual driving =======================
static void manualService() {
  int16_t t, s; uint8_t pan, tilt; uint32_t ms;
  portENTER_CRITICAL(&phoneMux); t = phone.t; s = phone.s; pan = phone.pan; tilt = phone.tilt; ms = phone.ms; portEXIT_CRITICAL(&phoneMux);
  bool fresh = ms && (millis() - ms < PHONE_TIMEOUT_MS);
  if (fresh) setCamera(pan, tilt);   // camera can move in any mode

  if (mode == MODE_IDLE && fresh && (abs(t) > 80 || abs(s) > 80) && probeJob == PJ_NONE) {
    mode = MODE_MANUAL; setStatus("manual");
  }
  if (mode != MODE_MANUAL) return;
  if (!fresh) { setDrive(0, 0); return; }
  setDrive(t, s);
}

// ======================= web =======================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncUDP udp;

static void wsMessage(const char* data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return;
  const char* c = doc["c"] | "";
  if (!strcmp(c, "d")) {   // stick, 20 times a second
    portENTER_CRITICAL(&phoneMux);
    phone.t = constrain((int)(doc["t"] | 0), -1000, 1000);
    phone.s = constrain((int)(doc["s"] | 0), -1000, 1000);
    phone.pan = constrain((int)(doc["p"] | 90), 0, 180);
    phone.tilt = constrain((int)(doc["ti"] | 90), 0, 180);
    phone.ms = millis();
    portEXIT_CRITICAL(&phoneMux);
    phoneSeenMs = millis();
    return;
  }
  if (!strcmp(c, "stop")) pushCommand(C_STOP);
  else if (!strcmp(c, "start")) pushCommand(C_START);
  else if (!strcmp(c, "pause")) pushCommand(C_PAUSE);
  else if (!strcmp(c, "manual")) pushCommand(C_MANUAL);
  else if (!strcmp(c, "probe")) pushCommand(C_PROBE);
  else if (!strcmp(c, "raise")) pushCommand(C_RAISE);
  else if (!strcmp(c, "home")) pushCommand(C_HOME);
  else if (!strcmp(c, "horn")) pushCommand(C_HORN);
  else if (!strcmp(c, "wp_here")) pushCommand(C_WP_HERE, doc["probe"] | 1);
  else if (!strcmp(c, "wp_clear")) pushCommand(C_WP_CLEAR);
}

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) wsMessage((const char*)data, len);
  } else if (type == WS_EVT_DISCONNECT) {
    portENTER_CRITICAL(&phoneMux); phone.t = 0; phone.s = 0; portEXIT_CRITICAL(&phoneMux);
  }
}

// UDP drive packet if I make an app: 'A' 'B', int16 throttle, int16 steer, uint8 pan, uint8 tilt (8 bytes)
static void udpPacket(AsyncUDPPacket& p) {
  if (p.length() != 8 || p.data()[0] != 'A' || p.data()[1] != 'B') return;
  int16_t t, s; memcpy(&t, p.data() + 2, 2); memcpy(&s, p.data() + 4, 2);
  portENTER_CRITICAL(&phoneMux);
  phone.t = constrain(t, -1000, 1000); phone.s = constrain(s, -1000, 1000);
  phone.pan = min<uint8_t>(p.data()[6], 180); phone.tilt = min<uint8_t>(p.data()[7], 180);
  phone.ms = millis();
  portEXIT_CRITICAL(&phoneMux);
  phoneSeenMs = millis();
}

static void sendTelemetry() {
  if (!ws.count()) return;
  static char buf[2600];
  int n = 0;
  auto add = [&](const char* fmt, ...) {
    if (n >= (int)sizeof buf - 1) return;
    va_list ap; va_start(ap, fmt); n += vsnprintf(buf + n, sizeof buf - n, fmt, ap); va_end(ap);
  };
  const char* modes[] = {"idle", "manual", "mission"};
  add("{\"mode\":\"%s\",\"step\":\"%s\",\"msg\":\"%s\",\"batt\":%.2f,\"wpi\":%u,\"wpn\":%u,", modes[mode],
      STEP_NAMES[step], statusMsg, battmV / 1000.0f, wpIndex, wpCount);
  add("\"lat\":%.7f,\"lon\":%.7f,\"sats\":%u,\"hdg\":%.0f,\"hdgOk\":%d,\"roll\":%.1f,\"pitch\":%.1f,",
      gps.location.lat(), gps.location.lng(), (unsigned)gps.satellites.value(), heading(), headingValid, roll, pitch);
  add("\"usL\":%u,\"usR\":%u,\"probe\":%u,\"probeMm\":%u,\"mflags\":%u,\"lm\":%d,\"la\":%d,\"sim\":%u,\"rssi\":%u,",
      motion.us_left_mm, motion.us_right_mm, motion.probe_state, motion.probe_mm, motion.flags,
      motionLink.alive(500), audioLink.alive(3000), audio.sim, audio.rssi);
  add("\"airT\":%s,\"hum\":%s,\"press\":%s,\"lux\":%s,", isnan(airTemp) ? "null" : String(airTemp, 1).c_str(),
      isnan(humidity) ? "null" : String(humidity, 0).c_str(), isnan(pressure) ? "null" : String(pressure, 1).c_str(),
      isnan(lux) ? "null" : String(lux, 0).c_str());
  add("\"cam\":\"%s\",", camLabel);
  if (lastSoil.valid)
    add("\"soil\":{\"m\":%.1f,\"t\":%.1f,\"ec\":%u,\"ph\":%.1f,\"n\":%u,\"p\":%u,\"k\":%u,\"at\":\"%s\"},", lastSoil.moisture,
        lastSoil.temp, lastSoil.ec, lastSoil.ph, lastSoil.n, lastSoil.p, lastSoil.k, lastSoil.at);
  add("\"wps\":[");
  for (int i = 0; i < wpCount; i++) add("%s[%.7f,%.7f,%u]", i ? "," : "", wps[i].lat, wps[i].lon, wps[i].probe);
  add("]}");
  ws.textAll(buf);
}

static void webBegin() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, 4);
  WiFi.setSleep(false);
  Serial.printf("[wifi] AP %s on ch%d, http://%s\n", AP_SSID, AP_CHANNEL, WiFi.softAPIP().toString().c_str());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/html; charset=utf-8", WEB_PAGE); });

  // download all the readings
  server.on("/log.csv", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!LittleFS.exists("/log.csv")) { r->send(404, "text/plain", "no readings yet"); return; }
    r->send(LittleFS, "/log.csv", "text/csv", true);
  });
  server.on("/api/log", HTTP_DELETE, [](AsyncWebServerRequest* r) { LittleFS.remove("/log.csv"); r->send(200, "text/plain", "cleared"); });

  // mission as JSON: [{"lat":30.0,"lon":31.2,"probe":1}, ...]
  server.on("/api/mission", HTTP_GET, [](AsyncWebServerRequest* r) {
    String s = "[";
    for (int i = 0; i < wpCount; i++) s += String(i ? "," : "") + "{\"lat\":" + String(wps[i].lat, 7) + ",\"lon\":" + String(wps[i].lon, 7) + ",\"probe\":" + wps[i].probe + "}";
    r->send(200, "application/json", s + "]");
  });
  auto* missionPost = new AsyncCallbackJsonWebHandler("/api/mission", [](AsyncWebServerRequest* r, JsonVariant& json) {
    if (mode == MODE_MISSION) { r->send(409, "text/plain", "pause the mission first"); return; }
    JsonArray a = json.as<JsonArray>();
    int n = 0;
    for (JsonObject o : a) {
      if (n >= MAX_WP) break;
      stagedWps[n++] = {o["lat"] | 0.0, o["lon"] | 0.0, (uint8_t)(o["probe"] | 1)};
    }
    portENTER_CRITICAL(&wpMux); stagedCount = n; portEXIT_CRITICAL(&wpMux);
    r->send(200, "text/plain", String(n) + " waypoints");
  });
  server.addHandler(missionPost);

  // ESP32-CAM asks what mode to be in and sends what it saw
  server.on("/api/cam", HTTP_GET, [](AsyncWebServerRequest* r) { camSeenMs = millis(); r->send(200, "text/plain", camScan ? "scan" : "drive"); });
  server.on("/api/cam/result", HTTP_GET, [](AsyncWebServerRequest* r) {
    camSeenMs = millis();
    String label = r->hasParam("label") ? r->getParam("label")->value() : "";
    String conf = r->hasParam("conf") ? r->getParam("conf")->value() : "";
    snprintf(camLabel, sizeof camLabel, "%s %s", label.c_str(), conf.c_str());
    for (char* c = camLabel; *c; c++) if (*c == '"' || *c == '\\' || *c == ',' || *c < 32) *c = '_';   // so it can't break the JSON or the CSV
    r->send(200, "text/plain", "ok");
  });
  server.onNotFound([](AsyncWebServerRequest* r) { r->send(404, "text/plain", "not found"); });
  server.begin();

  if (udp.listen(UDP_DRIVE_PORT)) udp.onPacket(udpPacket);
}

// ======================= links in =======================
static void linksService() {
  while (motionLink.poll()) if (motionLink.type == MSG_MOTION_STATE) motionLink.get(motion);

  while (audioLink.poll()) {
    if (audioLink.type == MSG_AUDIO_STATE) audioLink.get(audio);
    else if (audioLink.type == MSG_SMS_RECEIVED) { TextMsg t; if (audioLink.get(t)) { t.text[59] = 0; smsCommand(t.text); } }
  }

  while (displayLink.poll()) {
    UiAction a;
    if (displayLink.type != MSG_UI_ACTION || !displayLink.get(a)) continue;
    switch (a.action) {
      case UI_STOP: pushCommand(C_STOP); break;
      case UI_START_MISSION: pushCommand(C_START); break;
      case UI_PAUSE: pushCommand(C_PAUSE); break;
      case UI_PROBE_NOW: pushCommand(C_PROBE); break;
      case UI_RAISE_PROBE: pushCommand(C_RAISE); break;
      case UI_HORN: pushCommand(C_HORN); break;
      case UI_ADD_WAYPOINT_HERE: pushCommand(C_WP_HERE, 1); break;
    }
  }

  while (Serial2.available()) gps.encode(Serial2.read());
}

static void sendDisplay() {
  DisplayState d = {};
  d.mode = mode; d.mission_step = step; d.wp_index = wpIndex; d.wp_count = wpCount;
  d.batt_mV = (uint16_t)battmV;
  d.lat_e7 = (int32_t)(gps.location.lat() * 1e7); d.lon_e7 = (int32_t)(gps.location.lng() * 1e7);
  d.sats = gps.satellites.value();
  d.probe_state = motion.probe_state;
  d.us_left_mm = motion.us_left_mm; d.us_right_mm = motion.us_right_mm;
  d.roll_d10 = roll * 10; d.pitch_d10 = pitch * 10;
  d.air_temp_d10 = isnan(airTemp) ? INT16_MIN : (int16_t)(airTemp * 10);
  d.humidity = isnan(humidity) ? 255 : (uint8_t)humidity;
  d.lux = isnan(lux) ? 0xFFFF : (uint16_t)min(lux, 65534.0f);
  if (lastSoil.valid) {
    d.moisture_d10 = lastSoil.moisture * 10; d.soil_temp_d10 = lastSoil.temp * 10; d.ec = lastSoil.ec;
    d.ph_d10 = lastSoil.ph * 10; d.n = lastSoil.n; d.p = lastSoil.p; d.k = lastSoil.k;
  }
  d.flags = (motionLink.alive(500) ? DISP_FLAG_MOTION_LINK : 0) | (audioLink.alive(3000) ? DISP_FLAG_AUDIO_LINK : 0) |
            (gpsGood() ? DISP_FLAG_GPS_FIX : 0) | (millis() - phoneSeenMs < 2000 ? DISP_FLAG_PHONE : 0) |
            (audio.sim == SIM_REGISTERED ? DISP_FLAG_SIM : 0) | (lastSoil.valid ? DISP_FLAG_HAS_READING : 0);
  displayLink.send(MSG_DISPLAY, d);
}

// ======================= setup / loop =======================
void setup() {
  Serial.begin(115200);
  Serial.println("\nAgriBot SENSORS (hub)");

  motionTxLock = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(16, sizeof(Command));

  Serial1.begin(115200, SERIAL_8N1, LINK_MOTION_RX, LINK_MOTION_TX);
  Serial2.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  swAudio.begin(AUDIO_LINK_BAUD, EspSoftwareSerial::SWSERIAL_8N1, LINK_AUDIO_RX, LINK_AUDIO_TX, false, 256);
  swDisplay.begin(DISPLAY_LINK_BAUD, EspSoftwareSerial::SWSERIAL_8N1, DISPLAY_RX, DISPLAY_TX, false, 256);
  swSoil.begin(SOIL_BAUD, EspSoftwareSerial::SWSERIAL_8N1, RS485_RX, RS485_TX, false, 64);

  loopBeatMs = millis();
  xTaskCreatePinnedToCore(motionTxTask, "motionTx", 3072, nullptr, 5, nullptr, 1);

  analogSetPinAttenuation(BATT_SENSE, ADC_11db);
  battmV = analogReadMilliVolts(BATT_SENSE) * BATT_DIVIDER;

  if (!LittleFS.begin(true)) Serial.println("[fs] LittleFS failed");
  loadMission();

  setStatus("hold still: calibrating gyro");
  sensorsBegin();   // ~1 s, don't touch it
  webBegin();
  setStatus("ready");

  enableLoopWDT();
}

void loop() {
  loopBeatMs = millis();

  linksService();
  imuService();
  envService();
  headingService();

  Command c;
  while (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) handleCommand(c);

  // mission uploaded from POST /api/mission
  if (stagedCount >= 0) {
    portENTER_CRITICAL(&wpMux);
    int n = stagedCount; stagedCount = -1;
    portEXIT_CRITICAL(&wpMux);
    if (mode != MODE_MISSION) {
      memcpy(wps, stagedWps, sizeof(Waypoint) * n); wpCount = n; wpIndex = 0; stepTo(MS_IDLE);
      saveMission(); setStatus("mission loaded, %d points", n);
    }
  }

  manualService();
  probeService();
  missionService();
  if (mode == MODE_IDLE && probeJob == PJ_NONE) setDrive(0, 0);

  // after boot (or if Motion resets) Motion doesn't know where the probe is
  // and the wheels stay locked until it's homed
  if (motionLink.alive(500) && motion.probe_state == PROBE_UNHOMED && millis() - lastHomeSent > 30000) {
    lastHomeSent = millis();
    probeSend(PROBE_CMD_HOME);
    setStatus("homing probe");
  }

  // battery warnings
  if (battmV > 5000 && battmV < BATT_LOW_MV && !lowBattWarned) {
    lowBattWarned = true; play(CLIP_LOW_BATTERY); sms("AgriBot: battery low %.1fV", battmV / 1000);
  } else if (battmV > BATT_LOW_MV + 300) lowBattWarned = false;

  uint32_t now = millis();
  static uint32_t lastTel = 0, lastDisp = 0, lastClean = 0, lastPrint = 0;
  if (now - lastTel >= 200) { lastTel = now; sendTelemetry(); }
  if (now - lastDisp >= 250) { lastDisp = now; sendDisplay(); }
  if (now - lastClean >= 2000) { lastClean = now; ws.cleanupClients(); }
  if (now - lastPrint >= 2000) {
    lastPrint = now;
    Serial.printf("mode %u step %s | gps %s sats %u | hdg %.0f%s | batt %.2fV | motion %s rx%lu bad%lu | audio %s | disp %s | heap %u\n",
                  mode, STEP_NAMES[step], gpsGood() ? "OK" : "--", (unsigned)gps.satellites.value(), heading(),
                  headingValid ? "" : "?", battmV / 1000, motionLink.alive(500) ? "up" : "DOWN", motionLink.rxCount,
                  motionLink.badCount, audioLink.alive(3000) ? "up" : "DOWN", displayLink.alive(3000) ? "up" : "down",
                  (unsigned)ESP.getFreeHeap());
  }
}
