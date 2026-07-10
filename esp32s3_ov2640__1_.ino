/*
 * ESP32-S3 + OV2640  카메라 스트리밍 서버
 * - /        : 접속 정보 페이지
 * - /stream  : MJPEG 실시간 영상  (cma.htm 의 "카메라 켜기" 가 여기에 연결됨)
 * - /capture : 사진 1장 (JPEG)
 *
 * Arduino IDE 설정
 *   보드      : ESP32S3 Dev Module
 *   PSRAM     : OPI PSRAM  (반드시 켤 것)
 *   Partition : Huge APP (3MB No OTA)
 *   보드 매니저: esp32 by Espressif 3.x
 */

#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ESPmDNS.h>
#include "esp_http_server.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ================== 카메라 번호 (보드마다 이 숫자만 변경) ==================
#define CAM_ID  1     // 1~10 : 보드마다 1,2,3 ... 10 으로 변경해서 업로드
// 접속 주소:  http://camera-1.local  (번호에 맞게)
// =========================================================================

// ================== WiFi 설정 (여러 개 등록 가능) ==================
WiFiMulti wifiMulti;
void setupWiFiList() {
  wifiMulti.addAP("TAEJIN",    "taejinpress");
  wifiMulti.addAP("taejin-A",  "taejinpress");
  wifiMulti.addAP("U+NetC2B3", "4151007221");
}
// ================================================================

// ================== Supabase 전송 설정 ==================
const char* SUPABASE_URL   = "https://omngtyewdaqpphnzeate.supabase.co";
const char* SUPABASE_TABLE = "motion_counts";
const char* SUPABASE_KEY   =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im9tbmd0eWV3ZGFxcHBobnplYXRlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzQ4ODk4ODksImV4cCI6MjA5MDQ2NTg4OX0.Zxur3t02JHtOcXforCMSwO3i2mMaf5sViE0tXXUC0-s";
const uint32_t REPORT_INTERVAL_MS = 60000;   // 1분마다 전송
// ================== 움직임 감지 파라미터 ==================
const uint32_t MD_CHECK_MS     = 250;   // 감지 주기(ms)  → 초당 4회 (안정 확인값)
const int      MD_LUMA_THRESH  = 12;    // 블록 밝기 변화 임계값(클수록 둔감)
const int      MD_BLOCK_THRESH = 8;     // 변한 블록 수 임계값(100블록 중, 클수록 둔감)
const uint32_t MD_COOLDOWN_MS  = 200;   // 한 번 움직임 후 재카운트 대기(연속 움직임=1회)
const bool     MD_DEBUG        = false;  // 시리얼에 감지 상태 출력(테스트 후 false)
// =======================================================

// ===== OV2640 핀맵 (제네릭 ESP32-S3 카메라 보드 / Freenove 배열) =====
// 화면이 검게 나오면 이 부분을 보드 배열에 맞게 조정
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    4
#define SIOC_GPIO_NUM    5
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM   13
// ====================================================================

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t server = NULL;

// ---------- 루트 페이지 ----------
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  String ip = WiFi.localIP().toString();
  String html =
    "<!doctype html><meta charset='utf-8'><body style='font-family:sans-serif;text-align:center'>"
    "<h2>Camera " + String(CAM_ID) + " (ESP32-S3 OV2640)</h2>"
    "<p><a href='/stream'>실시간 영상 (/stream)</a></p>"
    "<p><a href='/capture'>사진 캡처 (/capture)</a></p>"
    "<img src='/stream' style='max-width:100%;border:1px solid #ccc'>"
    "<p style='color:#888'>camera-" + String(CAM_ID) + ".local  /  IP: " + ip + "</p></body>";
  return httpd_resp_send(req, html.c_str(), html.length());
}

// ---------- 카메라 번호 반환 (/id) ----------
static esp_err_t id_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  String s = String(CAM_ID);
  return httpd_resp_send(req, s.c_str(), s.length());
}

// ---------- 접속 정보 반환 (/info) : {"id":1,"ssid":"TAEJIN","ip":"192.168.0.51","rssi":-55} ----------
static esp_err_t info_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  String json = "{\"id\":" + String(CAM_ID) +
                ",\"ssid\":\"" + WiFi.SSID() + "\"" +
                ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
                ",\"rssi\":" + String(WiFi.RSSI()) + "}";
  return httpd_resp_send(req, json.c_str(), json.length());
}

// ---------- 사진 1장 ----------
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// ---------- MJPEG 스트림 ----------
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t jpg_len = 0;
  uint8_t *jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; }
    else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
        esp_camera_fb_return(fb); fb = NULL;
        if (!ok) res = ESP_FAIL;
      } else {
        jpg_len = fb->len;
        jpg_buf = fb->buf;
      }
    }
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 64, _STREAM_PART, jpg_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len);

    if (fb)          { esp_camera_fb_return(fb); fb = NULL; jpg_buf = NULL; }
    else if (jpg_buf){ free(jpg_buf); jpg_buf = NULL; }

    if (res != ESP_OK) break;
  }
  return res;
}

void startServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;

  httpd_uri_t index_uri   = { "/",        HTTP_GET, index_handler,   NULL };
  httpd_uri_t stream_uri  = { "/stream",  HTTP_GET, stream_handler,  NULL };
  httpd_uri_t capture_uri = { "/capture", HTTP_GET, capture_handler, NULL };
  httpd_uri_t id_uri      = { "/id",      HTTP_GET, id_handler,      NULL };
  httpd_uri_t info_uri    = { "/info",    HTTP_GET, info_handler,    NULL };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &stream_uri);
    httpd_register_uri_handler(server, &capture_uri);
    httpd_register_uri_handler(server, &id_uri);
    httpd_register_uri_handler(server, &info_uri);
  }
}

// ================== 움직임 감지 ==================
#define GX 10          // 가로 블록 수
#define GY 10          // 세로 블록 수
static int      prevBlk[GX * GY];
static bool     mdInit    = false;
static bool     wasMoving = false;
static uint32_t motionCount   = 0;   // 이번 주기 누적 카운트
static uint32_t lastCheckMs   = 0;
static uint32_t lastMotionMs  = 0;
static uint32_t lastReportMs  = 0;

// JPEG 프레임을 축소·그레이스케일 블록 평균으로 변환해 이전과 비교
void checkMotion() {
  if (millis() - lastCheckMs < MD_CHECK_MS) return;
  lastCheckMs = millis();

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;

  int ow = fb->width  / 8;   // JPG_SCALE_8X (1/8 축소) 후 크기
  int oh = fb->height / 8;
  if (ow <= 0 || oh <= 0) { esp_camera_fb_return(fb); return; }

  uint8_t *rgb = (uint8_t *)heap_caps_malloc(ow * oh * 2, MALLOC_CAP_SPIRAM);
  if (!rgb) rgb = (uint8_t *)malloc(ow * oh * 2);   // PSRAM 실패 시 일반 메모리
  if (!rgb) { esp_camera_fb_return(fb); if (MD_DEBUG) Serial.println("[MD] malloc 실패"); return; }

  bool ok = jpg2rgb565(fb->buf, fb->len, rgb, JPG_SCALE_8X);
  esp_camera_fb_return(fb);
  if (!ok) { free(rgb); if (MD_DEBUG) Serial.println("[MD] jpg 변환 실패"); return; }

  // 블록별 밝기 평균 계산
  long sum[GX * GY]; int cnt[GX * GY];
  memset(sum, 0, sizeof(sum));
  memset(cnt, 0, sizeof(cnt));
  uint16_t *px = (uint16_t *)rgb;
  for (int y = 0; y < oh; y++) {
    int gy = y * GY / oh;
    for (int x = 0; x < ow; x++) {
      int gx = x * GX / ow;
      uint16_t p = px[y * ow + x];
      int r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
      int luma = (r << 1) + g + (b << 1);   // 대략적 밝기
      sum[gy * GX + gx] += luma;
      cnt[gy * GX + gx]++;
    }
  }
  free(rgb);

  int curBlk[GX * GY];
  int changed = 0;
  for (int i = 0; i < GX * GY; i++) {
    curBlk[i] = cnt[i] ? (int)(sum[i] / cnt[i]) : 0;
    if (mdInit && abs(curBlk[i] - prevBlk[i]) > MD_LUMA_THRESH) changed++;
  }
  memcpy(prevBlk, curBlk, sizeof(prevBlk));

  if (!mdInit) { mdInit = true; return; }   // 첫 프레임은 기준

  if (MD_DEBUG && changed > 0)
    Serial.printf("[MD] changed=%d (기준 %d)\n", changed, MD_BLOCK_THRESH);

  bool moving = (changed >= MD_BLOCK_THRESH);
  if (moving) {
    lastMotionMs = millis();
    if (!wasMoving) {
      motionCount++;
      wasMoving = true;   // 움직임 시작 시 +1
      if (MD_DEBUG) Serial.printf("[MD] ★ 카운트 +1  (누적 %u)\n", motionCount);
    }
  } else if (wasMoving && (millis() - lastMotionMs > MD_COOLDOWN_MS)) {
    wasMoving = false;   // 멈춤 확정
  }
}

// 누적 카운트를 Supabase 로 전송
void sendToSupabase(uint32_t count) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();                 // 사내망용: 인증서 검증 생략
  HTTPClient https;
  String url = String(SUPABASE_URL) + "/rest/v1/" + SUPABASE_TABLE;
  if (!https.begin(client, url)) return;
  https.addHeader("Content-Type", "application/json");
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  https.addHeader("Prefer", "return=minimal");
  String body = "{\"camera_id\":" + String(CAM_ID) + ",\"count\":" + String(count) + "}";
  int code = https.POST(body);
  Serial.printf("[Supabase] cam%d count=%u -> HTTP %d\n", CAM_ID, count, code);
  https.end();
}
// ================================================

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size   = FRAMESIZE_SVGA;   // 800x600
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 2;

  if (!psramFound()) {           // PSRAM 없으면 저해상도로 강등
    config.frame_size   = FRAMESIZE_VGA;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("카메라 초기화 실패 0x%x\n", err);
    return;
  }

  // 상하 반전만 적용. 좌우까지 뒤집으려면 set_hmirror 를 1 로.
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 0);

  // 오탐 방지: 자동 노출/게인/화이트밸런스 고정 (밝기 출렁임 제거)
  s->set_whitebal(s, 0);       // AWB 끔
  s->set_awb_gain(s, 0);
  s->set_exposure_ctrl(s, 0);  // 자동 노출 끔
  s->set_aec2(s, 0);
  s->set_gain_ctrl(s, 0);      // 자동 게인 끔
  s->set_agc_gain(s, 0);
  s->set_aec_value(s, 300);    // 고정 노출값(0~1200, 환경에 맞게 조정)
  s->set_gainceiling(s, GAINCEILING_2X);
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);

  setupWiFiList();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  Serial.print("WiFi 연결 중");
  while (wifiMulti.run() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("연결됨: "); Serial.println(WiFi.SSID());

  // mDNS: IP 가 바뀌어도 camera-N.local 로 접속
  String host = "camera-" + String(CAM_ID);
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("http", "tcp", 80);
  }

  startServer();

  Serial.printf("Camera %d  준비 완료\n", CAM_ID);
  Serial.print("접속 주소:  http://");
  Serial.print(WiFi.localIP());
  Serial.printf("   또는   http://camera-%d.local\n", CAM_ID);

  lastReportMs = millis();
}

void loop() {
  checkMotion();                         // 움직임 감지 + 카운트

  if (millis() - lastReportMs >= REPORT_INTERVAL_MS) {
    lastReportMs = millis();
    uint32_t c = motionCount;
    motionCount = 0;                     // 주기마다 리셋
    sendToSupabase(c);                   // 0이어도 기록(가동 여부 확인용)
  }

  delay(5);
}
