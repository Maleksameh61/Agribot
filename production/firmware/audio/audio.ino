// AgriBot - AUDIO ESP32
//
// Plays the sound clips off the microSD through the 2 MAX98357As, and runs
// the SIM800L: texts me when Sensors tells it to, and passes texts from my
// number back to Sensors.
//
// SD card: FAT32, files in the root called 01.wav, 02.wav ... (the list is in
// agrilink.h). 16-bit PCM WAV, mono is fine. In Audacity export as
// "WAV (Microsoft) signed 16-bit PCM".
//
// Board: ESP32 Dev Module, ESP32 core 3.x (uses the new IDF 5 I2S driver)

#include <SPI.h>
#include <SD.h>
#include "driver/i2s_std.h"
#include "pins_audio.h"
#include "agrilink.h"

// ======================= CONFIG =======================
#define OWNER_NUMBER   "+201000000000"   // my number with +20, nobody else can text it commands
#define DEFAULT_VOLUME 70                // 0..100
#define LINK_BAUD      38400             // must match AUDIO_LINK_BAUD in sensors.ino
#define SIM_BAUD       9600
#define SD_SPI_HZ      10000000

// ======================= audio player =======================
AgriLink sensorsLink(Serial2);

i2s_chan_handle_t i2sTx = nullptr;
QueueHandle_t playQueue;           // uint8_t clip numbers, 0 = stop
volatile uint8_t nowPlaying = 0;
volatile uint8_t volumePct = DEFAULT_VOLUME;
volatile bool stopRequested = false;
bool sdOk = false;
uint32_t currentRate = 0;

static void i2sSetup(uint32_t rate) {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear = true;   // if it runs out of data it plays silence instead of buzzing
  ESP_ERROR_CHECK(i2s_new_channel(&cc, &i2sTx, nullptr));

  i2s_std_config_t sc = {};
  sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
  sc.gpio_cfg.ws   = (gpio_num_t)I2S_LRCLK;
  sc.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
  sc.gpio_cfg.din  = I2S_GPIO_UNUSED;
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2sTx, &sc));
  ESP_ERROR_CHECK(i2s_channel_enable(i2sTx));
  currentRate = rate;
}

static void i2sSetRate(uint32_t rate) {
  if (rate == currentRate) return;
  i2s_channel_disable(i2sTx);
  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  i2s_channel_reconfig_std_clock(i2sTx, &clk);
  i2s_channel_enable(i2sTx);
  currentRate = rate;
}

static void writeSilence(uint32_t ms) {
  static int16_t zeros[256] = {0};
  uint32_t frames = currentRate * ms / 1000;
  while (frames) {
    uint32_t n = min<uint32_t>(frames, 128);   // 128 stereo frames per block
    size_t w;
    i2s_channel_write(i2sTx, zeros, n * 4, &w, portMAX_DELAY);
    frames -= n;
  }
}

struct WavInfo { uint32_t rate; uint16_t channels; uint16_t bits; uint32_t dataStart; uint32_t dataLen; };

static bool parseWav(File& f, WavInfo& w) {
  uint8_t h[12];
  if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
  bool gotFmt = false;
  while (f.available()) {
    uint8_t ch[8];
    if (f.read(ch, 8) != 8) return false;
    uint32_t sz = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
    if (!memcmp(ch, "fmt ", 4)) {
      uint8_t fm[16];
      if (sz < 16 || f.read(fm, 16) != 16) return false;
      uint16_t fmtTag = fm[0] | (fm[1] << 8);
      w.channels = fm[2] | (fm[3] << 8);
      w.rate = fm[4] | (fm[5] << 8) | (fm[6] << 16) | ((uint32_t)fm[7] << 24);
      w.bits = fm[14] | (fm[15] << 8);
      if (fmtTag != 1) return false;          // PCM only
      f.seek(f.position() + sz - 16 + (sz & 1));
      gotFmt = true;
    } else if (!memcmp(ch, "data", 4)) {
      w.dataStart = f.position();
      w.dataLen = sz;
      return gotFmt;
    } else {
      f.seek(f.position() + sz + (sz & 1));   // skip LIST and other junk chunks
    }
  }
  return false;
}

static void playClip(uint8_t clip) {
  char path[12];
  snprintf(path, sizeof path, "/%02u.wav", clip);
  File f = SD.open(path);
  if (!f) { Serial.printf("[audio] %s not found\n", path); return; }
  WavInfo w = {};
  if (!parseWav(f, w) || w.bits != 16 || w.channels < 1 || w.channels > 2 || w.rate < 8000 || w.rate > 48000) {
    Serial.printf("[audio] %s: not 16-bit PCM mono/stereo\n", path);
    f.close();
    return;
  }
  f.seek(w.dataStart);
  i2sSetRate(w.rate);
  nowPlaying = clip;
  stopRequested = false;

  digitalWrite(AMP_SD, HIGH);   // amps on, then a bit of silence so it doesn't pop
  writeSilence(15);

  static int16_t in[512];
  static int16_t out[1024];
  uint32_t left = w.dataLen;
  while (left && !stopRequested) {
    if (uxQueueMessagesWaiting(playQueue)) break;   // new clip came in, drop this one
    size_t want = min<uint32_t>(left, sizeof in);
    int got = f.read((uint8_t*)in, want);
    if (got <= 0) break;
    left -= got;
    int samples = got / 2;
    int32_t vol = volumePct * 256 / 100;
    int frames;
    if (w.channels == 1) {
      frames = samples;
      for (int i = 0; i < samples; i++) { int16_t s = (in[i] * vol) >> 8; out[2 * i] = s; out[2 * i + 1] = s; }
    } else {
      frames = samples / 2;
      for (int i = 0; i < frames * 2; i++) out[i] = (in[i] * vol) >> 8;
    }
    size_t wr;
    i2s_channel_write(i2sTx, out, frames * 4, &wr, portMAX_DELAY);
  }
  f.close();
  writeSilence(30);
  if (!uxQueueMessagesWaiting(playQueue)) digitalWrite(AMP_SD, LOW);   // amps off when nothing's playing
  nowPlaying = 0;
}

static void playerTask(void*) {
  for (;;) {
    uint8_t clip;
    if (xQueueReceive(playQueue, &clip, portMAX_DELAY) == pdTRUE && clip && sdOk) playClip(clip);
  }
}

// ======================= SIM800L =======================
// one AT command at a time and never wait for it. every line that comes
// back goes through simLine(), including stuff the SIM800L sends on its own
// like +CMT (new text) and RDY (it rebooted)
HardwareSerial& sim = Serial1;

enum SimPhase { SP_PROBE, SP_INIT, SP_IDLE, SP_SMS_PROMPT, SP_SMS_BODY };
SimPhase simPhase = SP_PROBE;
uint8_t simState = SIM_STARTING;
uint8_t simRssi = 99;
bool simBusy = false;          // waiting for OK/ERROR
uint32_t simCmdAt = 0, simTimeout = 0;
uint8_t simFails = 0;
uint8_t initStep = 0;
uint32_t lastPoll = 0;
bool nextLineIsSms = false;
char smsFrom[24];

const char* const INIT_CMDS[] = {"ATE0", "AT+CMGF=1", "AT+CSCS=\"GSM\"", "AT+CNMI=2,2,0,0,0", "AT+CMGD=1,4"};
#define INIT_COUNT (sizeof(INIT_CMDS) / sizeof(INIT_CMDS[0]))

#define SMS_QUEUE 4
char smsQueue[SMS_QUEUE][61];
uint8_t smsHead = 0, smsCount = 0;
char simLineBuf[200];
uint8_t simLineLen = 0;

static void simSend(const char* cmd, uint32_t timeoutMs) {
  sim.print(cmd); sim.print("\r");
  simBusy = true; simCmdAt = millis(); simTimeout = timeoutMs;
}

static void queueSms(const char* text) {
  if (smsCount >= SMS_QUEUE) { Serial.println("[sim] SMS queue full, dropped"); return; }
  uint8_t slot = (smsHead + smsCount) % SMS_QUEUE;
  strlcpy(smsQueue[slot], text, sizeof smsQueue[slot]);
  smsCount++;
}

static bool sameNumber(const char* a, const char* b) {
  // last 10 digits only so +2010... and 010... count as the same
  char da[24], db[24]; int na = 0, nb = 0;
  for (; *a && na < 23; a++) if (isdigit(*a)) da[na++] = *a;
  for (; *b && nb < 23; b++) if (isdigit(*b)) db[nb++] = *b;
  if (na < 10 || nb < 10) return false;
  return memcmp(da + na - 10, db + nb - 10, 10) == 0;
}

static void simCommandDone(bool ok) {
  simBusy = false;
  if (ok) simFails = 0; else simFails++;
  if (simPhase == SP_PROBE) {
    if (ok) { simPhase = SP_INIT; initStep = 0; }
  } else if (simPhase == SP_INIT) {
    if (++initStep >= INIT_COUNT) { simPhase = SP_IDLE; simState = SIM_READY; Serial.println("[sim] ready"); }
  } else if (simPhase == SP_SMS_BODY) {
    simPhase = SP_IDLE;
    if (ok) { smsHead = (smsHead + 1) % SMS_QUEUE; smsCount--; Serial.println("[sim] SMS sent"); }
    else Serial.println("[sim] SMS failed, will retry");
  } else if (simPhase == SP_SMS_PROMPT) {
    simPhase = SP_IDLE;   // got ERROR instead of the > prompt
  }
  if (simFails >= 4 && simPhase != SP_PROBE) {    // it probably browned out and rebooted
    Serial.println("[sim] not answering, restarting init");
    simPhase = SP_PROBE; simState = SIM_STARTING; simFails = 0;
  }
}

static void simLine(char* line) {
  if (!*line) return;
  if (nextLineIsSms) {
    nextLineIsSms = false;
    if (sameNumber(smsFrom, OWNER_NUMBER)) {
      Serial.printf("[sim] SMS from owner: %s\n", line);
      sensorsLink.sendText(MSG_SMS_RECEIVED, line);
    } else Serial.printf("[sim] ignored SMS from %s\n", smsFrom);
    return;
  }
  if (!strncmp(line, "+CMT:", 5)) {          // +CMT: "+2010...","","26/11/03,20:10:00+08"
    char* q1 = strchr(line, '"');
    char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
    if (q1 && q2) { *q2 = 0; strlcpy(smsFrom, q1 + 1, sizeof smsFrom); nextLineIsSms = true; }
    return;
  }
  if (!strncmp(line, "+CSQ:", 5)) { simRssi = atoi(line + 5); return; }
  if (!strncmp(line, "+CREG:", 6)) {
    char* c = strchr(line, ',');
    int st = c ? atoi(c + 1) : 0;
    if (simState >= SIM_READY) simState = (st == 1 || st == 5) ? SIM_REGISTERED : SIM_READY;
    return;
  }
  if (!strcmp(line, "RDY")) { simPhase = SP_PROBE; simState = SIM_STARTING; simBusy = false; return; }
  if (!strcmp(line, "OK") || !strncmp(line, "+CMGS:", 6)) { if (!strcmp(line, "OK")) simCommandDone(true); return; }
  if (!strcmp(line, "ERROR") || !strncmp(line, "+CMS ERROR", 10) || !strncmp(line, "+CME ERROR", 10)) {
    simCommandDone(false);
    return;
  }
}

static void simService() {
  while (sim.available()) {
    char c = sim.read();
    if (simPhase == SP_SMS_PROMPT && c == '>') {
      // the > has no newline after it so catch it here
      sim.print(smsQueue[smsHead]);
      sim.write(0x1A);                       // Ctrl+Z = send
      simPhase = SP_SMS_BODY;
      simCmdAt = millis(); simTimeout = 60000;
      simLineLen = 0;
      continue;
    }
    if (c == '\n') { simLineBuf[simLineLen] = 0; simLine(simLineBuf); simLineLen = 0; }
    else if (c != '\r' && simLineLen < sizeof simLineBuf - 1) simLineBuf[simLineLen++] = c;
  }

  uint32_t now = millis();
  if (simBusy) {
    if (now - simCmdAt > simTimeout) simCommandDone(false);
    return;
  }

  switch (simPhase) {
    case SP_PROBE:
      if (now - simCmdAt > 1000) simSend("AT", 800);
      break;
    case SP_INIT:
      simSend(INIT_CMDS[initStep], 3000);
      break;
    case SP_IDLE:
      if (smsCount && simState == SIM_REGISTERED) {
        char c[48];
        snprintf(c, sizeof c, "AT+CMGS=\"%s\"", OWNER_NUMBER);
        simSend(c, 5000);
        simPhase = SP_SMS_PROMPT;
      } else if (now - lastPoll > 15000) {
        lastPoll = now;
        static bool which = false;
        simSend((which = !which) ? "AT+CSQ" : "AT+CREG?", 2000);
      }
      break;
    default: break;
  }
}

// ======================= link to Sensors =======================
static void linkService() {
  while (sensorsLink.poll()) {
    switch (sensorsLink.type) {
      case MSG_PLAY: {
        PlayCmd p;
        if (sensorsLink.get(p)) {
          if (p.volume <= 100) volumePct = p.volume;
          uint8_t clip = p.clip;
          xQueueSend(playQueue, &clip, 0);
        }
        break;
      }
      case MSG_STOP_AUDIO: {
        stopRequested = true;
        xQueueReset(playQueue);
        break;
      }
      case MSG_SMS_SEND: {
        TextMsg t;
        if (sensorsLink.get(t)) { t.text[sizeof t.text - 1] = 0; queueSms(t.text); }
        break;
      }
    }
  }
}

void setup() {
  pinMode(AMP_SD, OUTPUT); digitalWrite(AMP_SD, LOW);   // amps off until audio starts
  Serial.begin(115200);
  Serial.println("\nAgriBot AUDIO");

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, SPI, SD_SPI_HZ);
  Serial.printf("[sd] %s\n", sdOk ? "ok" : "NOT FOUND (check card is FAT32, and the 5V/3V3 feed)");

  i2sSetup(16000);
  playQueue = xQueueCreate(4, sizeof(uint8_t));
  xTaskCreatePinnedToCore(playerTask, "player", 6144, nullptr, 3, nullptr, 0);

  sim.begin(SIM_BAUD, SERIAL_8N1, SIM_RX, SIM_TX);
  Serial2.begin(LINK_BAUD, SERIAL_8N1, LINK_SENSORS_RX, LINK_SENSORS_TX);

  uint8_t boot = CLIP_BOOT;
  xQueueSend(playQueue, &boot, 0);
}

void loop() {
  linkService();
  simService();

  static uint32_t lastState = 0;
  if (millis() - lastState >= 1000) {
    lastState = millis();
    AudioState s;
    s.sim = simState;
    s.rssi = simRssi;
    s.playing = nowPlaying;
    s.sd_ok = sdOk;
    s.sms_queue = smsCount;
    sensorsLink.send(MSG_AUDIO_STATE, s);
  }
  delay(1);
}
