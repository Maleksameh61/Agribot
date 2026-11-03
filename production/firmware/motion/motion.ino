// AgriBot - MOTION ESP32
//
// This one runs the 4 gearmotors (through the 2 MDD10As), the NEMA 17 that
// pushes the soil probe down, the 2 camera servos, and the 2 HC-SR04s.
// It only listens to Sensors.
//
// It stops on its own without waiting for Sensors if:
//   - Sensors goes quiet for 150 ms (deadman)
//   - something is closer than 30 cm in front (it can still reverse and turn)
//   - the probe isn't all the way up (so I don't drag it through the dirt)
//   - the loop hangs (watchdog resets it, and the 10k pull-downs on the PWM
//     pins keep the drivers off while it reboots)
//
// Board: ESP32 Dev Module, ESP32 core 3.x

#include "driver/ledc.h"
#include "soc/gpio_reg.h"
#include <Preferences.h>
#include "pins_motion.h"
#include "agrilink.h"

// ======================= CONFIG (check these on the bench) =======================

// Sensors sends every 50 ms, so this is 3 missed frames in a row
#define DEADMAN_MS 150

// I'm assuming MD1 is the left side and MD2 is the right. Wheels in the air,
// stick forward, and flip invert on whichever wheel goes backwards.
struct MotorCh { int pwm; int dir; bool leftSide; bool invert; ledc_channel_t ch; };
MotorCh motors[4] = {
  {MD1_PWM1, MD1_DIR1, true,  false, LEDC_CHANNEL_0},  // left front?
  {MD1_PWM2, MD1_DIR2, true,  false, LEDC_CHANNEL_1},  // left rear?
  {MD2_PWM1, MD2_DIR1, false, true,  LEDC_CHANNEL_2},  // right front? (mirrored mount)
  {MD2_PWM2, MD2_DIR2, false, true,  LEDC_CHANNEL_3},  // right rear?
};
#define MOTOR_PWM_HZ    20000   // MDD10A max, and you can't hear it
#define MOTOR_PWM_BITS  LEDC_TIMER_10_BIT
#define MOTOR_MAX_DUTY  1023
#define RAMP_PER_SEC    2500    // 0 to full in 0.4 s so 4 motors don't all slam on at once
#define MIN_DUTY_PCT    15      // below this the worm gears just hum and don't move

// Ultrasonics
#define OBSTACLE_STOP_MM 300    // 30 cm
#define US_PERIOD_MS     40     // take turns so they don't hear each other's ping

// Servos (MG90S)
#define SERVO_MIN_US 500
#define SERVO_MAX_US 2400
#define SERVO_DEG_PER_SEC 180   // so the camera doesn't snap around
#define PAN_CENTER  90
#define TILT_CENTER 90

// Probe stepper. steps per mm = (200 * microsteps) / (2 * pi * pinion radius)
// 1/8 step with a 6 mm pinion is 1600 / 37.7 = 42.4
// I still need to tell it 50 mm, measure what it really did, and fix the radius.
#define MICROSTEPS         8
#define PINION_RADIUS_MM   6.0f
#define STEPS_PER_MM       ((200.0f * MICROSTEPS) / (2.0f * PI * PINION_RADIUS_MM))
#define PROBE_MAX_MM       200     // never go deeper than this
#define PROBE_SPEED_MM_S   15.0f
#define PROBE_ACCEL_MM_S2  40.0f
#define HOME_TRAVEL_MM     (PROBE_MAX_MM + 20)  // homing = go up this far no matter what
#define HOME_SPEED_MM_S    8.0f    // slow so it doesn't slam the top
// Rack and pinion doesn't lock itself like the worm gears, so the driver stays
// on the whole time to hold it. There's no limit switch, so homing just drives
// up into the top and lets the stepper skip. Keep the DRV8825 current low
// (I'm starting at 0.8-1 A) so that doesn't chew up the rack.
#define PROBE_INTERLOCK    1       // 1 = wheels don't move unless the probe is up

// Battery
#define BATT_EMA 0.05f

// ======================= state =======================
AgriLink sensorsLink(Serial1);

DriveCmd cmd = {0, 0, PAN_CENTER, TILT_CENTER, 0};
uint32_t lastCmdMs = 0;

float leftOut = 0, rightOut = 0;   // ramped side speeds, -1000..1000
uint8_t flags = 0;

// ultrasonics
volatile uint32_t usRiseUs[2] = {0, 0};
volatile uint32_t usWidthUs[2] = {0, 0};
volatile bool usGotEcho[2] = {false, false};
uint16_t usMm[2] = {0xFFFF, 0xFFFF};
uint16_t usHist[2][3] = {{0xFFFF, 0xFFFF, 0xFFFF}, {0xFFFF, 0xFFFF, 0xFFFF}};
uint8_t usNext = 0;
uint32_t usLastTrig = 0;
bool usFired = false;

// servos
float panPos = PAN_CENTER, tiltPos = TILT_CENTER;

// stepper
uint8_t probeState = PROBE_UNHOMED;
long stepPos = 0, stepTarget = 0;
float stepV = 0, stepVmax = 0, stepAccel = 0;
uint32_t lastStepUs = 0;
int stepDir = 0;
bool faultPinUsable = true;
uint32_t faultLowSince = 0;

float battmV = 0;

// save "probe is up" so it doesn't grind into the top for 25 s every time I
// turn it on. gets cleared as soon as it starts going down, so if the power
// dies halfway it does a real home next time
Preferences nvs;
static void rememberUp(bool up) { nvs.putBool("up", up); }

// ======================= PWM via the IDF LEDC driver =======================
// motors on timer 0 at 20 kHz, servos on timer 1 at 50 Hz. needs 2 different
// frequencies, that's why I'm not using analogWrite
static void pwmTimer(ledc_timer_t t, uint32_t hz, ledc_timer_bit_t bits) {
  ledc_timer_config_t c = {};
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.duty_resolution = bits;
  c.timer_num = t;
  c.freq_hz = hz;
  c.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&c));
}
static void pwmChannel(ledc_channel_t ch, int pin, ledc_timer_t t) {
  ledc_channel_config_t c = {};
  c.gpio_num = pin;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = ch;
  c.timer_sel = t;
  c.duty = 0;
  c.hpoint = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}
static void pwmWrite(ledc_channel_t ch, uint32_t duty) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

#define SERVO_PAN_CH  LEDC_CHANNEL_4
#define SERVO_TILT_CH LEDC_CHANNEL_5
static void servoWrite(ledc_channel_t ch, float deg) {
  deg = constrain(deg, 0.0f, 180.0f);
  float us = SERVO_MIN_US + (SERVO_MAX_US - SERVO_MIN_US) * deg / 180.0f;
  pwmWrite(ch, (uint32_t)(us * 65535.0f / 20000.0f));  // 16-bit over a 20 ms period
}

// ======================= motors =======================
static void setMotor(const MotorCh& m, float speed) {
  bool fwd = speed >= 0;
  if (m.invert) fwd = !fwd;
  float mag = fabsf(speed);
  uint32_t duty = 0;
  if (mag > 5) {
    float pct = MIN_DUTY_PCT + (100.0f - MIN_DUTY_PCT) * (mag / 1000.0f);
    duty = (uint32_t)(MOTOR_MAX_DUTY * pct / 100.0f);
  }
  digitalWrite(m.dir, fwd ? HIGH : LOW);
  pwmWrite(m.ch, duty);
}

static float approach(float cur, float target, float maxStep) {
  if (target > cur + maxStep) return cur + maxStep;
  if (target < cur - maxStep) return cur - maxStep;
  return target;
}

static void motorsService(float dt) {
  flags &= ~(MOTION_FLAG_DEADMAN | MOTION_FLAG_OBSTACLE | MOTION_FLAG_PROBE_LOCK);

  float t = cmd.throttle, s = cmd.steer;

  bool deadman = (lastCmdMs == 0) || (millis() - lastCmdMs > DEADMAN_MS);
  if (deadman) { t = 0; s = 0; flags |= MOTION_FLAG_DEADMAN; }

  bool obstacle = (usMm[0] < OBSTACLE_STOP_MM) || (usMm[1] < OBSTACLE_STOP_MM);
  if (obstacle && t > 0 && !(cmd.flags & DRIVE_FLAG_IGNORE_OBSTACLE)) {
    t = 0;                       // turning in place and reversing still allowed
    flags |= MOTION_FLAG_OBSTACLE;
  }

#if PROBE_INTERLOCK
  if (probeState != PROBE_UP && (t != 0 || s != 0)) {
    t = 0; s = 0;
    flags |= MOTION_FLAG_PROBE_LOCK;
  }
#endif

  // skid-steer mix
  float l = t + s, r = t - s;
  float big = max(fabsf(l), fabsf(r));
  if (big > 1000) { l = l * 1000 / big; r = r * 1000 / big; }

  if (deadman || (flags & MOTION_FLAG_PROBE_LOCK)) {
    leftOut = 0; rightOut = 0;   // worm gears lock anyway so just cut it
  } else {
    float step = RAMP_PER_SEC * dt;
    // slowing down can be fast, speeding up is ramped
    leftOut  = approach(leftOut,  l, (fabsf(l) < fabsf(leftOut))  ? step * 3 : step);
    rightOut = approach(rightOut, r, (fabsf(r) < fabsf(rightOut)) ? step * 3 : step);
  }

  for (auto& m : motors) setMotor(m, m.leftSide ? leftOut : rightOut);
}

// ======================= ultrasonics =======================
// reading the register directly because digitalRead is in flash, and if this
// fires while the NVS is writing it crashes
void IRAM_ATTR echoIsr0() {
  uint32_t now = micros();
  if (REG_READ(GPIO_IN_REG) & (1UL << US1_ECHO)) usRiseUs[0] = now;
  else { usWidthUs[0] = now - usRiseUs[0]; usGotEcho[0] = true; }
}
void IRAM_ATTR echoIsr1() {
  uint32_t now = micros();
  if (REG_READ(GPIO_IN_REG) & (1UL << US2_ECHO)) usRiseUs[1] = now;
  else { usWidthUs[1] = now - usRiseUs[1]; usGotEcho[1] = true; }
}

static uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) { uint16_t x = a; a = b; b = x; }
  if (b > c) { uint16_t x = b; b = c; c = x; }
  if (a > b) { uint16_t x = a; a = b; b = x; }
  return b;
}

static void ultrasonicService() {
  uint32_t now = millis();
  if (now - usLastTrig < US_PERIOD_MS) return;

  // collect the result of the sensor we fired last time
  if (usFired) {
    uint8_t i = usNext;
    uint16_t mm = 0xFFFF;
    if (usGotEcho[i]) {
      uint32_t w = usWidthUs[i];
      if (w > 100 && w < 25000) mm = (uint16_t)(w * 343UL / 2000UL);  // up to ~4.3 m
    }
    usHist[i][0] = usHist[i][1]; usHist[i][1] = usHist[i][2]; usHist[i][2] = mm;
    usMm[i] = median3(usHist[i][0], usHist[i][1], usHist[i][2]);  // median of 3 to kill random spikes
    usNext ^= 1;
  }

  // fire the next one
  uint8_t i = usNext;
  int trig = i ? US2_TRIG : US1_TRIG;
  usGotEcho[i] = false;
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  usLastTrig = now;
  usFired = true;
}

// ======================= servos =======================
static void servoService(float dt) {
  float step = SERVO_DEG_PER_SEC * dt;
  panPos  = approach(panPos,  cmd.pan,  step);
  tiltPos = approach(tiltPos, cmd.tilt, step);
  servoWrite(SERVO_PAN_CH, panPos);
  servoWrite(SERVO_TILT_CH, tiltPos);
}

// ======================= probe stepper =======================
// + steps = probe goes DOWN
static void stepperMoveTo(long target, float vmaxMmS) {
  stepTarget = target;
  stepVmax = vmaxMmS * STEPS_PER_MM;
  stepAccel = PROBE_ACCEL_MM_S2 * STEPS_PER_MM;
  int d = (stepTarget > stepPos) ? 1 : (stepTarget < stepPos ? -1 : 0);
  if (d != stepDir) { stepV = 0; stepDir = d; }
  digitalWrite(STEPPER_DIR, d > 0 ? HIGH : LOW);   // flip HIGH/LOW here if "down" goes up
  digitalWrite(STEPPER_EN, LOW);                   // driver on (active low)
  delayMicroseconds(2);                            // DIR setup time
}

static void stepperService() {
  if (stepPos == stepTarget) { stepV = 0; return; }
  if (stepV < 1) stepV = sqrtf(stepAccel) + 1;     // first step speed
  uint32_t now = micros();
  uint32_t interval = (uint32_t)(1e6f / stepV);
  if (now - lastStepUs < interval) return;
  lastStepUs = now;

  digitalWrite(STEPPER_STEP, HIGH);
  delayMicroseconds(3);                            // DRV8825 wants >= 1.9 us
  digitalWrite(STEPPER_STEP, LOW);
  stepPos += stepDir;

  // trapezoid: speed up, cruise, slow down before the target
  long remaining = labs(stepTarget - stepPos);
  float stopDist = stepV * stepV / (2 * stepAccel);
  float dtS = interval / 1e6f;
  if (remaining <= stopDist) stepV = max(stepV - stepAccel * dtS, sqrtf(stepAccel));
  else stepV = min(stepV + stepAccel * dtS, stepVmax);
}

static void probeCommand(const ProbeCmd& p) {
  switch (p.cmd) {
    case PROBE_CMD_STOP:
      stepTarget = stepPos;
      if (probeState == PROBE_LOWERING || probeState == PROBE_RAISING) probeState = PROBE_DOWN;
      if (probeState == PROBE_HOMING) probeState = PROBE_UNHOMED;
      break;
    case PROBE_CMD_LOWER: {
      if (probeState == PROBE_UNHOMED || probeState == PROBE_HOMING || probeState == PROBE_FAULT) break;
      if (fabsf(leftOut) > 1 || fabsf(rightOut) > 1) break;     // never while rolling
      uint16_t mm = min<uint16_t>(p.depth_mm, PROBE_MAX_MM);
      rememberUp(false);
      stepperMoveTo(lroundf(mm * STEPS_PER_MM), PROBE_SPEED_MM_S);
      probeState = PROBE_LOWERING;
      break;
    }
    case PROBE_CMD_RAISE:
      if (probeState == PROBE_UNHOMED || probeState == PROBE_FAULT) break;
      stepperMoveTo(0, PROBE_SPEED_MM_S);
      probeState = PROBE_RAISING;
      break;
    case PROBE_CMD_HOME:
      if (probeState == PROBE_FAULT) { probeState = PROBE_UNHOMED; flags &= ~MOTION_FLAG_STEPPER_FAULT; }
      rememberUp(false);
      stepPos = lroundf(HOME_TRAVEL_MM * STEPS_PER_MM);   // act like it's all the way down
      stepperMoveTo(0, HOME_SPEED_MM_S);                   // then go all the way up
      probeState = PROBE_HOMING;
      break;
  }
}

static void probeService() {
  // FAULT is open drain and 34 has no pull-up, see the check in setup()
  if (faultPinUsable && digitalRead(STEPPER_FAULT) == LOW) {
    if (!faultLowSince) faultLowSince = millis();
    if (millis() - faultLowSince > 50 && probeState != PROBE_FAULT) {
      stepTarget = stepPos;
      digitalWrite(STEPPER_EN, HIGH);
      probeState = PROBE_FAULT;
      flags |= MOTION_FLAG_STEPPER_FAULT;
      Serial.println("[probe] DRV8825 FAULT - stepper off. Send HOME to retry.");
    }
  } else faultLowSince = 0;

  if (stepPos != stepTarget) return;
  switch (probeState) {
    case PROBE_LOWERING: probeState = PROBE_DOWN; break;
    case PROBE_RAISING:  probeState = PROBE_UP; rememberUp(true); break;
    case PROBE_HOMING:   stepPos = 0; stepTarget = 0; probeState = PROBE_UP; rememberUp(true); break;
    default: break;
  }
}

// ======================= link =======================
static void linkService() {
  while (sensorsLink.poll()) {
    switch (sensorsLink.type) {
      case MSG_DRIVE: {
        DriveCmd c;
        if (sensorsLink.get(c)) {
          c.throttle = constrain(c.throttle, -1000, 1000);
          c.steer = constrain(c.steer, -1000, 1000);
          c.pan = min<uint8_t>(c.pan, 180);
          c.tilt = min<uint8_t>(c.tilt, 180);
          cmd = c;
          lastCmdMs = millis();
        }
        break;
      }
      case MSG_PROBE: {
        ProbeCmd p;
        if (sensorsLink.get(p)) probeCommand(p);
        break;
      }
    }
  }
}

static void sendState() {
  MotionState s;
  s.batt_mV = (uint16_t)battmV;
  s.us_left_mm = usMm[0];
  s.us_right_mm = usMm[1];
  s.left_out = (int16_t)leftOut;
  s.right_out = (int16_t)rightOut;
  s.probe_state = probeState;
  s.probe_mm = (uint16_t)max(0L, lroundf(stepPos / STEPS_PER_MM));
  s.flags = flags;
  sensorsLink.send(MSG_MOTION_STATE, s);
}

// ======================= setup / loop =======================
void setup() {
  // everything off first
  pinMode(STEPPER_EN, OUTPUT);   digitalWrite(STEPPER_EN, HIGH);   // stepper off
  pinMode(STEPPER_STEP, OUTPUT); digitalWrite(STEPPER_STEP, LOW);
  pinMode(STEPPER_DIR, OUTPUT);  digitalWrite(STEPPER_DIR, LOW);
  pinMode(STEPPER_FAULT, INPUT);
  for (auto& m : motors) { pinMode(m.dir, OUTPUT); digitalWrite(m.dir, LOW); }

  Serial.begin(115200);
  Serial.println("\nAgriBot MOTION");

  pwmTimer(LEDC_TIMER_0, MOTOR_PWM_HZ, MOTOR_PWM_BITS);
  pwmTimer(LEDC_TIMER_1, 50, LEDC_TIMER_16_BIT);
  for (auto& m : motors) pwmChannel(m.ch, m.pwm, LEDC_TIMER_0);
  pwmChannel(SERVO_PAN_CH, SERVO1_PWM, LEDC_TIMER_1);
  pwmChannel(SERVO_TILT_CH, SERVO2_PWM, LEDC_TIMER_1);
  servoWrite(SERVO_PAN_CH, panPos);
  servoWrite(SERVO_TILT_CH, tiltPos);

  pinMode(US1_TRIG, OUTPUT); digitalWrite(US1_TRIG, LOW);
  pinMode(US2_TRIG, OUTPUT); digitalWrite(US2_TRIG, LOW);
  pinMode(US1_ECHO, INPUT);
  pinMode(US2_ECHO, INPUT);
  attachInterrupt(digitalPinToInterrupt(US1_ECHO), echoIsr0, CHANGE);
  attachInterrupt(digitalPinToInterrupt(US2_ECHO), echoIsr1, CHANGE);

  analogSetPinAttenuation(BATT_SENSE, ADC_11db);
  battmV = analogReadMilliVolts(BATT_SENSE) * BATT_DIVIDER;

  // if FAULT is already low with the driver off, it's floating (no pull-up).
  // ignore it, otherwise the probe would randomly stop
  delay(5);
  if (digitalRead(STEPPER_FAULT) == LOW) {
    faultPinUsable = false;
    Serial.println("[probe] FAULT pin reads LOW at boot - no pull-up? Ignoring it.");
  }

  nvs.begin("motion", false);
  if (nvs.getBool("up", false)) {
    probeState = PROBE_UP;            // it was up when the power went off
    digitalWrite(STEPPER_EN, LOW);    // hold it there
    Serial.println("[probe] was up at power-off, skipping home");
  }

  Serial1.begin(115200, SERIAL_8N1, LINK_SENSORS_RX, LINK_SENSORS_TX);

  enableLoopWDT();   // if the loop hangs, reset instead of leaving the motors on
}

void loop() {
  static uint32_t lastUs = micros(), lastSend = 0, lastPrint = 0;
  uint32_t nowUs = micros();
  float dt = (nowUs - lastUs) / 1e6f;
  lastUs = nowUs;

  linkService();
  ultrasonicService();
  motorsService(dt);
  servoService(dt);
  stepperService();
  probeService();

  uint32_t now = millis();
  static uint32_t lastAdc = 0;
  if (now - lastAdc >= 20) {
    lastAdc = now;
    battmV += BATT_EMA * (analogReadMilliVolts(BATT_SENSE) * BATT_DIVIDER - battmV);
  }

  if (now - lastSend >= 50) { lastSend = now; sendState(); }
  if (now - lastPrint >= 1000) {
    lastPrint = now;
    Serial.printf("batt %.2fV  L %d R %d  us %u/%u mm  probe st%d %ldst  flags %02X  link rx %lu bad %lu\n",
                  battmV / 1000, (int)leftOut, (int)rightOut, usMm[0], usMm[1], probeState, stepPos,
                  flags, sensorsLink.rxCount, sensorsLink.badCount);
  }
}
