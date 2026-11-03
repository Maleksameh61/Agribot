// AgriBot - CYD touchscreen (ESP32-2432S028R)
//
// Shows what the rover is doing, plus big STOP / GO / PAUSE / PROBE / UP
// buttons. Talks to Sensors through the 4-pin cable on the CYD's P1 connector
// (VIN, TX, RX, GND). That's the same UART as the CYD's USB, so:
//   - unplug the cable from the rover before uploading
//   - no Serial.print anywhere in here, it would go down the cable
//
// STOP here goes CYD -> Sensors -> Motion, so it's a software stop, same as
// the phone. Not a real e-stop.
//
// First boot (or keep your finger on the screen while it turns on) it asks
// you to touch the 4 corners to calibrate, then saves it.
//
// Board: ESP32 Dev Module, ESP32 core 3.x, LovyanGFX.
// If the colours look wrong or it's mirrored, it's the 2-USB CYD with the
// ST7789, change Panel_ILI9341 to Panel_ST7789.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include "agrilink.h"

#define LINK_BAUD 38400   // must match DISPLAY_LINK_BAUD in sensors.ino

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 panel;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM light;
  lgfx::Touch_XPT2046 touch;
 public:
  LGFX() {
    { auto c = bus.config();
      c.spi_host = HSPI_HOST; c.spi_mode = 0; c.freq_write = 40000000; c.freq_read = 16000000;
      c.spi_3wire = false; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 14; c.pin_mosi = 13; c.pin_miso = 12; c.pin_dc = 2;
      bus.config(c); panel.setBus(&bus); }
    { auto c = panel.config();
      c.pin_cs = 15; c.pin_rst = -1; c.pin_busy = -1;
      c.panel_width = 240; c.panel_height = 320; c.offset_rotation = 0;
      c.readable = true; c.invert = false; c.rgb_order = false; c.dlen_16bit = false; c.bus_shared = false;
      panel.config(c); }
    { auto c = light.config();
      c.pin_bl = 21; c.invert = false; c.freq = 12000; c.pwm_channel = 7;
      light.config(c); panel.setLight(&light); }
    { auto c = touch.config();   // touch is on its own SPI pins on the CYD
      c.x_min = 300; c.x_max = 3900; c.y_min = 200; c.y_max = 3700;
      c.pin_int = 36; c.bus_shared = false; c.offset_rotation = 0;
      c.spi_host = VSPI_HOST; c.freq = 1000000;
      c.pin_sclk = 25; c.pin_mosi = 32; c.pin_miso = 39; c.pin_cs = 33;
      touch.config(c); panel.setTouch(&touch); }
    setPanel(&panel);
  }
};

LGFX lcd;
AgriLink sensorsLink(Serial);
Preferences prefs;

DisplayState st = {};
char statusText[61] = "waiting for Sensors...";
bool haveState = false, statusDirty = true;

// colours
#define C_BG   0x0861   // near-black green
#define C_CARD 0x10E3
#define C_TXT  TFT_WHITE
#define C_DIM  0x8C71
#define C_OK   0x3E6D
#define C_BAD  0xE249
#define C_WARN 0xF524

struct Button { int x, y, w, h; const char* label; uint8_t action; uint16_t color; };
Button buttons[] = {
  {0,   184, 88, 56, "STOP",  UI_STOP,          C_BAD},
  {93,  184, 53, 56, "GO",    UI_START_MISSION, 0x1B47},
  {151, 184, 53, 56, "PAUSE", UI_PAUSE,         0x2965},
  {209, 184, 53, 56, "PROBE", UI_PROBE_NOW,     0x2965},
  {267, 184, 53, 56, "UP",    UI_RAISE_PROBE,   0x2965},
};

static const char* const STEP_TEXT[] = {"idle", "waiting GPS", "find heading", "turning", "driving", "avoiding",
                                        "avoiding", "avoiding", "probing", "paused", "done", "ABORTED"};
static const char* const PROBE_TEXT[] = {"unhomed", "up", "lowering", "DOWN", "raising", "homing", "FAULT"};
static const char* const MODE_TEXT[] = {"IDLE", "MANUAL", "MISSION"};

static void drawButton(const Button& b, bool pressed) {
  lcd.fillRoundRect(b.x, b.y, b.w, b.h, 8, pressed ? TFT_WHITE : b.color);
  lcd.setTextColor(pressed ? TFT_BLACK : TFT_WHITE);
  lcd.setFont(b.action == UI_STOP ? &fonts::Font4 : &fonts::Font2);
  lcd.setTextDatum(middle_center);
  lcd.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2);
  lcd.setTextDatum(top_left);
}

static void drawStatic() {
  lcd.fillScreen(C_BG);
  lcd.fillRoundRect(0, 50, 157, 128, 8, C_CARD);
  lcd.fillRoundRect(163, 50, 157, 128, 8, C_CARD);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(C_DIM, C_CARD);
  lcd.drawString("SOIL (last)", 8, 54);
  lcd.drawString("ROVER", 171, 54);
  for (auto& b : buttons) drawButton(b, false);
}

#define field(x, y, fg, bg, ...) fieldW(x, y, fg, bg, 19, __VA_ARGS__)
// pad with spaces so a shorter number covers the old longer one
static void fieldW(int x, int y, uint16_t fg, uint16_t bg, int width, const char* fmt, ...) {
  char buf[40];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  char pad[41]; snprintf(pad, sizeof pad, "%-*.*s", width, width, buf);
  lcd.setTextColor(fg, bg);
  lcd.drawString(pad, x, y);
}

static void drawState() {
  bool linked = sensorsLink.alive(2000);
  lcd.setFont(&fonts::Font2);

  // top bar
  uint16_t modeCol = st.mode == MODE_MISSION ? C_OK : (st.mode == MODE_MANUAL ? C_WARN : C_DIM);
  lcd.setTextColor(modeCol, C_BG);
  lcd.setFont(&fonts::Font4);
  char top[32];
  snprintf(top, sizeof top, "%-8s", linked ? MODE_TEXT[min<uint8_t>(st.mode, 2)] : "NO LINK");
  lcd.drawString(top, 4, 2);
  lcd.setFont(&fonts::Font2);
  float v = st.batt_mV / 1000.0f;
  fieldW(220, 2, v < 11.6f ? C_BAD : C_TXT, C_BG, 9, "%5.2f V", v);
  fieldW(220, 18, C_DIM, C_BG, 10, "%s%s%s%s",
        (st.flags & DISP_FLAG_MOTION_LINK) ? "M " : "m ", (st.flags & DISP_FLAG_AUDIO_LINK) ? "A " : "a ",
        (st.flags & DISP_FLAG_PHONE) ? "P " : "  ", (st.flags & DISP_FLAG_SIM) ? "SIM" : "");
  if (st.wp_count) fieldW(120, 2, C_TXT, C_BG, 11, "wp %u/%u", min<int>(st.wp_index + 1, st.wp_count), st.wp_count);
  else fieldW(120, 2, C_DIM, C_BG, 11, "no wps");
  fieldW(120, 18, C_DIM, C_BG, 11, "%s", STEP_TEXT[min<uint8_t>(st.mission_step, 11)]);

  if (statusDirty || !linked) {
    statusDirty = false;
    char s[64]; snprintf(s, sizeof s, "%-40s", linked ? statusText : "check the cable to Sensors");
    lcd.setTextColor(linked ? C_TXT : C_BAD, C_BG);
    lcd.drawString(s, 4, 34);
  }

  // soil
  int y = 72;
  if (st.flags & DISP_FLAG_HAS_READING) {
    field(8, y, C_TXT, C_CARD, "Moist  %.1f %%", st.moisture_d10 / 10.0f); y += 16;
    field(8, y, C_TXT, C_CARD, "Temp   %.1f C", st.soil_temp_d10 / 10.0f); y += 16;
    field(8, y, C_TXT, C_CARD, "EC     %u uS", st.ec); y += 16;
    field(8, y, C_TXT, C_CARD, "pH     %.1f", st.ph_d10 / 10.0f); y += 16;
    field(8, y, C_TXT, C_CARD, "N %u P %u K %u", st.n, st.p, st.k); y += 16;
  } else {
    field(8, y, C_DIM, C_CARD, "no reading yet");
  }

  // rover
  y = 72;
  bool fix = st.flags & DISP_FLAG_GPS_FIX;
  field(171, y, fix ? C_OK : C_WARN, C_CARD, "GPS %s %u sats", fix ? "ok" : "--", st.sats); y += 16;
  field(171, y, st.probe_state == PROBE_DOWN || st.probe_state == PROBE_FAULT ? C_WARN : C_TXT, C_CARD,
        "Probe %s", PROBE_TEXT[min<uint8_t>(st.probe_state, 6)]); y += 16;
  char l[8], r[8];
  if (st.us_left_mm == 0xFFFF) strcpy(l, "--"); else snprintf(l, sizeof l, "%u", st.us_left_mm / 10);
  if (st.us_right_mm == 0xFFFF) strcpy(r, "--"); else snprintf(r, sizeof r, "%u", st.us_right_mm / 10);
  field(171, y, C_TXT, C_CARD, "Front %s/%s cm", l, r); y += 16;
  field(171, y, C_TXT, C_CARD, "Tilt %.0f/%.0f deg", st.roll_d10 / 10.0f, st.pitch_d10 / 10.0f); y += 16;
  if (st.air_temp_d10 != INT16_MIN) field(171, y, C_TXT, C_CARD, "Air %.1fC %u%%", st.air_temp_d10 / 10.0f, st.humidity);
  else field(171, y, C_DIM, C_CARD, "Air --");
  y += 16;
  if (st.lux != 0xFFFF) field(171, y, C_TXT, C_CARD, "Light %u lux", st.lux);
}

static void calibrate() {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE);
  lcd.setFont(&fonts::Font2);
  lcd.setTextDatum(middle_center);
  lcd.drawString("Touch the arrows in each corner", lcd.width() / 2, lcd.height() / 2);
  lcd.setTextDatum(top_left);
  uint16_t cal[8];
  lcd.calibrateTouch(cal, TFT_WHITE, TFT_BLACK, 20);
  prefs.putBytes("cal", cal, sizeof cal);
}

void setup() {
  Serial.begin(LINK_BAUD);   // UART0 = P1 = the cable to Sensors
  lcd.init();
  lcd.setRotation(1);        // sideways, USB on the left
  lcd.setBrightness(200);

  prefs.begin("agribot", false);
  uint16_t cal[8];
  int32_t tx, ty;
  bool holding = lcd.getTouchRaw(&tx, &ty);
  if (holding || prefs.getBytes("cal", cal, sizeof cal) != sizeof cal) calibrate();
  else lcd.setTouchCalibrate(cal);

  drawStatic();
}

void loop() {
  while (sensorsLink.poll()) {
    if (sensorsLink.type == MSG_DISPLAY && sensorsLink.get(st)) haveState = true;
    else if (sensorsLink.type == MSG_STATUS_TEXT) {
      TextMsg t;
      if (sensorsLink.get(t)) { t.text[59] = 0; strlcpy(statusText, t.text, sizeof statusText); statusDirty = true; }
    }
  }

  // buttons: send on press, white while held
  static int held = -1;
  static uint32_t lastSend = 0;
  int32_t x, y;
  bool touching = lcd.getTouch(&x, &y);
  int hit = -1;
  if (touching)
    for (int i = 0; i < (int)(sizeof buttons / sizeof buttons[0]); i++)
      if (x >= buttons[i].x && x < buttons[i].x + buttons[i].w && y >= buttons[i].y && y < buttons[i].y + buttons[i].h) hit = i;
  if (hit != held) {
    if (held >= 0) drawButton(buttons[held], false);
    if (hit >= 0) {
      drawButton(buttons[hit], true);
      UiAction a = {buttons[hit].action};
      sensorsLink.send(MSG_UI_ACTION, a);
      lastSend = millis();
    }
    held = hit;
  }
  // keep spamming STOP while it's held in case one gets lost
  if (held >= 0 && buttons[held].action == UI_STOP && millis() - lastSend > 200) {
    UiAction a = {UI_STOP};
    sensorsLink.send(MSG_UI_ACTION, a);
    lastSend = millis();
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 250) { lastDraw = millis(); drawState(); }
  delay(5);
}
