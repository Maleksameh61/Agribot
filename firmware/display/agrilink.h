// agrilink.h - how the ESP32s talk to each other over the UART wires
//
// Every sketch folder has its own copy of this file because the Arduino IDE
// won't include anything outside the sketch folder. I only edit the one in
// common/ and then run sync_common.sh. If the copies don't match, the boards
// read each other's messages wrong and you'll waste an evening on it.
//
// Frame: 0xA5 0x5A | type | len | payload | crc8
// crc8 is over type + len + payload (poly 0x07). All the chips are ESP32s so
// they're all little-endian, which means I just send the packed structs raw.

#pragma once
#include <Arduino.h>

#define AGRILINK_VERSION 1
#define AGRILINK_MAX_PAYLOAD 64

// ---------------- message types ----------------
enum : uint8_t {
  // Sensors -> Motion
  MSG_DRIVE        = 0x10,  // DriveCmd, sent at 20 Hz. This is also the deadman heartbeat
  MSG_PROBE        = 0x11,  // ProbeCmd

  // Motion -> Sensors
  MSG_MOTION_STATE = 0x20,  // MotionState, 20 Hz

  // Sensors -> Audio
  MSG_PLAY         = 0x30,  // PlayCmd
  MSG_STOP_AUDIO   = 0x31,  // no payload
  MSG_SMS_SEND     = 0x32,  // TextMsg, goes to OWNER_NUMBER

  // Audio -> Sensors
  MSG_AUDIO_STATE  = 0x40,  // AudioState, 1 Hz
  MSG_SMS_RECEIVED = 0x41,  // TextMsg, only from OWNER_NUMBER

  // Sensors -> Display
  MSG_DISPLAY      = 0x50,  // DisplayState, 4 Hz
  MSG_STATUS_TEXT  = 0x51,  // TextMsg, one status line

  // Display -> Sensors
  MSG_UI_ACTION    = 0x60,  // UiAction
};

// ---------------- payloads ----------------
// Drive flags
#define DRIVE_FLAG_IGNORE_OBSTACLE 0x01  // let Motion drive forward even with something in front

struct __attribute__((packed)) DriveCmd {
  int16_t throttle;   // -1000..1000, + = forward
  int16_t steer;      // -1000..1000, + = turn right
  uint8_t pan;        // servo degrees 0..180
  uint8_t tilt;       // servo degrees 0..180
  uint8_t flags;
};

enum : uint8_t { PROBE_CMD_STOP = 0, PROBE_CMD_LOWER = 1, PROBE_CMD_RAISE = 2, PROBE_CMD_HOME = 3 };

struct __attribute__((packed)) ProbeCmd {
  uint8_t  cmd;
  uint16_t depth_mm;  // only used by PROBE_CMD_LOWER
};

enum : uint8_t {
  PROBE_UNHOMED = 0,  // position unknown after boot, wheels locked until homed
  PROBE_UP      = 1,
  PROBE_LOWERING= 2,
  PROBE_DOWN    = 3,
  PROBE_RAISING = 4,
  PROBE_HOMING  = 5,
  PROBE_FAULT   = 6,  // DRV8825 FAULT pin went low
};

// MotionState flags
#define MOTION_FLAG_DEADMAN      0x01  // no DriveCmd in DEADMAN_MS, motors forced to 0
#define MOTION_FLAG_OBSTACLE     0x02  // forward motion blocked by an ultrasonic
#define MOTION_FLAG_PROBE_LOCK   0x04  // wheels locked because the probe isn't up
#define MOTION_FLAG_STEPPER_FAULT 0x08

struct __attribute__((packed)) MotionState {
  uint16_t batt_mV;
  uint16_t us_left_mm;   // HC-SR04 #1, 0xFFFF = nothing in range
  uint16_t us_right_mm;  // HC-SR04 #2
  int16_t  left_out;     // what the left side is actually doing, -1000..1000
  int16_t  right_out;
  uint8_t  probe_state;
  uint16_t probe_mm;     // how far down the probe is
  uint8_t  flags;
};

struct __attribute__((packed)) PlayCmd {
  uint8_t clip;    // plays /NN.wav off the microSD, see CLIP_* below
  uint8_t volume;  // 0..100, 255 = keep current
};

// Clip numbers. Record these yourself and save them as /01.wav, /02.wav ...
// (16-bit PCM WAV, mono or stereo, any sample rate up to 48 kHz)
enum : uint8_t {
  CLIP_BOOT = 1, CLIP_MISSION_START = 2, CLIP_WAYPOINT = 3, CLIP_PROBE_DOWN = 4,
  CLIP_READING_SAVED = 5, CLIP_OBSTACLE = 6, CLIP_ERROR = 7, CLIP_MISSION_DONE = 8,
  CLIP_LOW_BATTERY = 9, CLIP_HORN = 10,
};

enum : uint8_t { SIM_NONE = 0, SIM_STARTING = 1, SIM_READY = 2, SIM_REGISTERED = 3 };

struct __attribute__((packed)) AudioState {
  uint8_t sim;      // SIM_*
  uint8_t rssi;     // AT+CSQ value 0..31, 99 = unknown
  uint8_t playing;  // clip number, 0 = silent
  uint8_t sd_ok;
  uint8_t sms_queue;
};

struct __attribute__((packed)) TextMsg {
  char text[60];   // zero-terminated
};

enum : uint8_t { MODE_IDLE = 0, MODE_MANUAL = 1, MODE_MISSION = 2 };

// DisplayState flags
#define DISP_FLAG_MOTION_LINK 0x01
#define DISP_FLAG_AUDIO_LINK  0x02
#define DISP_FLAG_GPS_FIX     0x04
#define DISP_FLAG_PHONE       0x08
#define DISP_FLAG_SIM         0x10
#define DISP_FLAG_HAS_READING 0x20

struct __attribute__((packed)) DisplayState {
  uint8_t  mode;         // MODE_*
  uint8_t  mission_step; // MissionStep in sensors.ino
  uint8_t  wp_index;
  uint8_t  wp_count;
  uint16_t batt_mV;
  int32_t  lat_e7;
  int32_t  lon_e7;
  uint8_t  sats;
  uint8_t  probe_state;
  uint16_t us_left_mm;
  uint16_t us_right_mm;
  int16_t  roll_d10;     // degrees x10
  int16_t  pitch_d10;
  int16_t  air_temp_d10;
  uint8_t  humidity;
  uint16_t lux;
  // last soil reading
  uint16_t moisture_d10; // % x10
  int16_t  soil_temp_d10;
  uint16_t ec;           // uS/cm
  uint8_t  ph_d10;
  uint16_t n, p, k;      // mg/kg
  uint8_t  flags;        // DISP_FLAG_*
};

enum : uint8_t {
  UI_STOP = 1, UI_START_MISSION = 2, UI_PAUSE = 3, UI_PROBE_NOW = 4, UI_RAISE_PROBE = 5,
  UI_HORN = 6, UI_ADD_WAYPOINT_HERE = 7,
};

struct __attribute__((packed)) UiAction {
  uint8_t action;
};

static_assert(sizeof(DisplayState) <= AGRILINK_MAX_PAYLOAD, "DisplayState too big");

// ---------------- framing ----------------
static inline uint8_t agrilink_crc8(uint8_t crc, const uint8_t* d, size_t n) {
  while (n--) {
    crc ^= *d++;
    for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  }
  return crc;
}

class AgriLink {
 public:
  explicit AgriLink(Stream& s) : io(s) {}

  void send(uint8_t type, const void* payload, uint8_t len) {
    if (len > AGRILINK_MAX_PAYLOAD) return;
    uint8_t hdr[4] = {0xA5, 0x5A, type, len};
    uint8_t crc = agrilink_crc8(0, hdr + 2, 2);
    crc = agrilink_crc8(crc, (const uint8_t*)payload, len);
    io.write(hdr, 4);
    if (len) io.write((const uint8_t*)payload, len);
    io.write(crc);
    txCount++;
  }
  template <typename T> void send(uint8_t type, const T& msg) { send(type, &msg, sizeof(T)); }
  void sendText(uint8_t type, const char* s) {
    TextMsg m; memset(&m, 0, sizeof m); strlcpy(m.text, s, sizeof m.text); send(type, m);
  }

  // Call often. Returns true once per complete, valid frame; read it from type/len/buf.
  bool poll() {
    while (io.available()) {
      uint8_t b = io.read();
      switch (st) {
        case 0: st = (b == 0xA5) ? 1 : 0; break;
        case 1: st = (b == 0x5A) ? 2 : (b == 0xA5 ? 1 : 0); break;
        case 2: type = b; st = 3; break;
        case 3:
          len = b; pos = 0;
          if (len > AGRILINK_MAX_PAYLOAD) { st = 0; badCount++; }
          else st = len ? 4 : 5;
          break;
        case 4: buf[pos++] = b; if (pos >= len) st = 5; break;
        case 5: {
          st = 0;
          uint8_t hdr[2] = {type, len};
          uint8_t crc = agrilink_crc8(agrilink_crc8(0, hdr, 2), buf, len);
          if (crc == b) { lastRxMs = millis(); rxCount++; return true; }
          badCount++;
          break;
        }
      }
    }
    return false;
  }

  // Copy the last frame into a struct if the length matches.
  template <typename T> bool get(T& out) const {
    if (len != sizeof(T)) return false;
    memcpy(&out, buf, sizeof(T));
    return true;
  }
  bool alive(uint32_t timeoutMs) const { return lastRxMs && (millis() - lastRxMs) < timeoutMs; }

  uint8_t type = 0, len = 0;
  uint8_t buf[AGRILINK_MAX_PAYLOAD];
  uint32_t lastRxMs = 0, rxCount = 0, txCount = 0, badCount = 0;

 private:
  Stream& io;
  uint8_t st = 0, pos = 0;
};
