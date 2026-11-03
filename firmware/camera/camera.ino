// AgriBot - ESP32-CAM (AI-Thinker, OV2640)
//
// Joins the AgriBot WiFi from Sensors as 192.168.4.10 and:
//   - streams video at http://192.168.4.10:81/stream (the phone page shows it)
//   - one picture at http://192.168.4.10/capture
//   - asks Sensors every second if it should be in "drive" or "scan" mode.
//     scan = rover parked with the probe in the ground, so it runs my Edge
//     Impulse FOMO model and sends the result to Sensors, which puts it in
//     the log next to that soil reading.
//
// Edge Impulse: Deployment -> Arduino library in EI, add the zip in the IDE,
// set USE_EDGE_IMPULSE to 1 and EI_HEADER to <project_name>_inferencing.h
//
// Board: AI Thinker ESP32-CAM, ESP32 core 3.x, PSRAM on.
// Use "Huge APP" partition if the model doesn't fit.

#define USE_EDGE_IMPULSE 0
#define EI_HEADER <AgriBot_inferencing.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#if USE_EDGE_IMPULSE
#include EI_HEADER
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "img_converters.h"
#endif

#define AP_SSID "AgriBot"        // same as in sensors.ino
#define AP_PASS "agribot2026"
#define HUB_URL "http://192.168.4.1"
IPAddress camIp(192, 168, 4, 10), gateway(192, 168, 4, 1), subnet(255, 255, 255, 0);

#define STREAM_FPS_MAX 12        // QVGA at ~12 fps is about all the ESP32 WiFi can handle
#define JPEG_QUALITY   14        // lower = better but bigger

// AI-Thinker ESP32-CAM pins
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM     5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22
#define FLASH_LED      4

volatile bool scanMode = false;

static bool cameraBegin() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_QVGA;      // 320x240, also what the EI model wants
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count = psramFound() ? 2 : 1;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;   // always the newest frame, not an old one from the queue
  return esp_camera_init(&c) == ESP_OK;
}

// ======================= MJPEG stream =======================
#define BOUNDARY "agribotframe"
static esp_err_t streamHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char part[64];
  uint32_t frameMs = 1000 / STREAM_FPS_MAX;
  for (;;) {
    uint32_t t0 = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    int n = snprintf(part, sizeof part, "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    esp_err_t r = httpd_resp_send_chunk(req, part, n);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (r != ESP_OK) break;            // phone closed it
    uint32_t took = millis() - t0;
    if (took < frameMs) vTaskDelay(pdMS_TO_TICKS(frameMs - took));
  }
  return ESP_OK;
}

static esp_err_t captureHandler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t r = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return r;
}

static void serversBegin() {
  httpd_handle_t h80 = nullptr, h81 = nullptr;
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 80; c.ctrl_port = 32768;
  if (httpd_start(&h80, &c) == ESP_OK) {
    httpd_uri_t cap = {};
    cap.uri = "/capture"; cap.method = HTTP_GET; cap.handler = captureHandler;
    httpd_register_uri_handler(h80, &cap);
  }
  c.server_port = 81; c.ctrl_port = 32769;
  if (httpd_start(&h81, &c) == ESP_OK) {
    httpd_uri_t st = {};
    st.uri = "/stream"; st.method = HTTP_GET; st.handler = streamHandler;
    httpd_register_uri_handler(h81, &st);
  }
}

// ======================= Edge Impulse =======================
#if USE_EDGE_IMPULSE
static uint8_t* rgb = nullptr;   // 320x240x3, PSRAM

static int eiGetData(size_t offset, size_t length, float* out) {
  size_t px = offset * 3;
  for (size_t i = 0; i < length; i++, px += 3)   // fmt2rgb888 gives BGR not RGB
    out[i] = (rgb[px + 2] << 16) + (rgb[px + 1] << 8) + rgb[px];
  return 0;
}

// label = what it saw, conf = how sure
static bool runModel(String& label, float& conf) {
  if (!rgb) rgb = (uint8_t*)ps_malloc(320 * 240 * 3);
  if (!rgb) return false;
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return false;
  bool ok = fmt2rgb888(fb->buf, fb->len, fb->format, rgb);
  esp_camera_fb_return(fb);
  if (!ok) return false;
  ei::image::processing::crop_and_interpolate_rgb888(rgb, 320, 240, rgb, EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);

  signal_t sig;
  sig.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  sig.get_data = &eiGetData;
  ei_impulse_result_t res = {};
  if (run_classifier(&sig, &res, false) != EI_IMPULSE_OK) return false;

  label = "none"; conf = 0;
#if EI_CLASSIFIER_OBJECT_DETECTION == 1
  // FOMO: take the most confident thing and count how many of it there are
  int count = 0;
  for (uint32_t i = 0; i < res.bounding_boxes_count; i++) {
    auto& b = res.bounding_boxes[i];
    if (b.value <= 0) continue;
    if (b.value > conf) { conf = b.value; label = b.label; }
  }
  for (uint32_t i = 0; i < res.bounding_boxes_count; i++)
    if (res.bounding_boxes[i].value > 0 && label == res.bounding_boxes[i].label) count++;
  if (count > 1) label += "_x" + String(count);
#else
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++)
    if (res.classification[i].value > conf) { conf = res.classification[i].value; label = res.classification[i].label; }
#endif
  return true;
}
#endif

// ======================= talk to Sensors =======================
static void hubTask(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.setTimeout(800);
      if (http.begin(HUB_URL "/api/cam") && http.GET() == 200) scanMode = http.getString().startsWith("scan");
      http.end();

#if USE_EDGE_IMPULSE
      if (scanMode) {
        String label; float conf;
        if (runModel(label, conf)) {
          label.replace(" ", "_");
          String url = String(HUB_URL "/api/cam/result?label=") + label + "&conf=" + String(conf, 2);
          if (http.begin(url)) { http.GET(); http.end(); }
          Serial.printf("[ei] %s %.2f\n", label.c_str(), conf);
        }
      }
#endif
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   // the WiFi spike keeps tripping brownout on weak 5V
  Serial.begin(115200);
  Serial.println("\nAgriBot CAMERA");
  pinMode(FLASH_LED, OUTPUT); digitalWrite(FLASH_LED, LOW);

  if (!cameraBegin()) { Serial.println("[cam] init failed - check the ribbon cable"); delay(3000); ESP.restart(); }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.config(camIp, gateway, subnet);
  WiFi.setAutoReconnect(true);
  WiFi.begin(AP_SSID, AP_PASS);
  Serial.print("[wifi] joining " AP_SSID);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(250); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? " ok" : " not yet, will keep trying");

  serversBegin();
  xTaskCreatePinnedToCore(hubTask, "hub", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 5000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) { WiFi.disconnect(); WiFi.begin(AP_SSID, AP_PASS); }
  }
  delay(100);
}
