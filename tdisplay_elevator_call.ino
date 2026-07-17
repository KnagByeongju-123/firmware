/*
  엘리베이터 제품 상차 알림 수신기 - LILYGO T-Display  v2.0
  ============================================================
  기존 안돈(andon_msg 폴링, taejincall)과 완전 분리된 별도 시스템

    구분          기존 안돈            본 기기
    ------------------------------------------------
    테이블        andon_msg            call_light (id='floor2')
    폴링          30초                 2초
    알림          메시지 표시          10초 알림 후 자동 해제
    ntfy          taejincall           taejin_qc_call

  [동작]
  - index.html El 클릭 → call_light.state=true
  - 2초 폴링 감지 → 10초 알림
      · 화면: 검은 배경 + 노란 글자 '엘리베이터 알림' 흐름
      · LED 점멸 + 부저 리듬음
  - 10초 경과 → 자동 해제 (state=false 복귀)
  - 짧게 터치      → 즉시 알림 해제
  - 길게 누름(1초) → 알림 해제 + ntfy 'taejin_qc_call' 에 '수작업검사실 콜' 전송
  - 대기화면: 검은 배경 + 흰 글자 '대기중' (글자만, 이미지 없음)

  [보드 설정 - Arduino IDE]
    Board: ESP32 Dev Module / Flash Size: 16MB
    Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
    ESP32 core 2.0.17

  [라이브러리]
    TFT_eSPI (Setup25_TTGO_T_Display.h), efont, ArduinoJson

  [결선 - USB 5V 급전, 부하는 전부 3.3V 직결]
    부저 적(+) → GPIO26  (3.3V 직접 구동, 12mA)
    부저 흑(−) → GND
    외부 LED   → GPIO25 + 680R → GND
    TTP223 OUT → GPIO17 / VCC → 3.3V / GND → GND
*/

#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>

#define efontEnableKr
#include <efont.h>
#include <efontEnableKr.h>

// ===== 핀 =====
#define BUZZ_PIN      26        // 부저(+) 직결, 3.3V 구동
#define LED_PIN       25        // 외부 LED
#define TOUCH_PIN     17        // TTP223

// ===== 부저 =====
#define BUZZ_CH       0
#define BUZZ_PWM_HZ   30000
#define BUZZ_RES      8
#define BUZZ_DUTY     255       // 음량 (255=최대). 3.3V 직결이라 최대로 시작, 크면 낮출 것

// 리듬: 따다다닥(60ms×4) → 쉼(620ms) 반복
const uint16_t PATTERN[] = { 60, 70, 60, 70, 60, 70, 60, 620 };
const uint8_t  PATTERN_N = sizeof(PATTERN) / sizeof(PATTERN[0]);

// ===== 동작 =====
#define POLL_MS       2000UL    // Supabase 폴링
#define ALARM_MS      10000UL   // 알림 유지 10초
#define BLINK_MS      350UL     // LED 점멸 주기
#define SCROLL_MS     30UL      // 글자 흐름 속도
#define SCROLL_STEP   2
#define LONG_PRESS_MS 1000UL    // 길게 누름 인정 시간 (1초)

// ===== 서버 =====
#define LIGHT_ID      "floor2"
const char* SB_URL   = "https://omngtyewdaqpphnzeate.supabase.co";
const char* SB_KEY   = "sb_publishable_9j2YkkL-7ul1TrhH-NjVdQ_vWDG2-1D";
const char* NTFY_URL = "https://ntfy.sh/taejin_qc_call";

// ===== 문구 =====
const char* MSG_ALARM = "엘리베이터 알림   ●   엘리베이터 알림   ●   ";
const char* MSG_IDLE  = "대기중";

// ===== 화면 =====
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
#define SCR_W 240
#define SCR_H 135
#define BAND_H 40

WiFiMulti wifiMulti;

bool alarmOn = false;
unsigned long alarmStart = 0, lastPoll = 0, lastBlink = 0, lastScroll = 0;
bool ledState = false, buzzState = false;
int  scrollX = 0, msgPixW = 0;

uint8_t  patIdx = 0;
unsigned long patNext = 0;

unsigned long msgUntil = 0;

// ---------- 부저 ----------
void buzz(bool on) {
  if (buzzState == on) return;
  buzzState = on;
  ledcWrite(BUZZ_CH, on ? BUZZ_DUTY : 0);
}

// ---------- UTF-8 → Unicode ----------
int utf8ToUnicode(const char* s, int i, uint16_t* out) {
  uint8_t c = s[i];
  if (c < 0x80)              { *out = c;                                    return i + 1; }
  else if ((c & 0xE0)==0xC0) { *out = ((c & 0x1F) << 6) | (s[i+1] & 0x3F);  return i + 2; }
  else if ((c & 0xF0)==0xE0) { *out = ((c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); return i + 3; }
  *out = '?';
  return i + 1;
}

int drawEfontSpr(TFT_eSprite* sp, const char* str, int x, int y, int scale, uint16_t color) {
  int i = 0, cx = x;
  byte font[32];
  while (str[i]) {
    uint16_t uni;
    i = utf8ToUnicode(str, i, &uni);
    getefontData(font, uni);
    bool wide = (uni >= 0x80);
    int gw = wide ? 16 : 8;
    for (int row = 0; row < 16; row++) {
      uint16_t bits = wide ? ((font[row*2] << 8) | font[row*2+1]) : (font[row*2] << 8);
      for (int col = 0; col < gw; col++) {
        if (bits & (0x8000 >> col)) {
          if (scale == 1) sp->drawPixel(cx + col, y + row, color);
          else            sp->fillRect(cx + col*scale, y + row*scale, scale, scale, color);
        }
      }
    }
    cx += gw * scale;
  }
  return cx - x;
}

int efontWidth(const char* str, int scale) {
  int i = 0, w = 0;
  while (str[i]) {
    uint16_t uni;
    i = utf8ToUnicode(str, i, &uni);
    w += (uni >= 0x80 ? 16 : 8) * scale;
  }
  return w;
}

// ---------- 화면 ----------
void drawCenterText(const char* msg, uint16_t color) {
  tft.fillScreen(TFT_BLACK);
  spr.setColorDepth(16);
  spr.createSprite(SCR_W, 32);
  spr.fillSprite(TFT_BLACK);
  int w = efontWidth(msg, 2);
  drawEfontSpr(&spr, msg, (SCR_W - w) / 2, 0, 2, color);
  spr.pushSprite(0, (SCR_H - 32) / 2);
  spr.deleteSprite();
}

void drawIdle() {
  tft.fillScreen(TFT_BLACK);
  spr.setColorDepth(16);
  spr.createSprite(SCR_W, 32);
  spr.fillSprite(TFT_BLACK);
  int w = efontWidth(MSG_IDLE, 2);
  drawEfontSpr(&spr, MSG_IDLE, (SCR_W - w) / 2, 0, 2, TFT_WHITE);
  spr.pushSprite(0, (SCR_H - 32) / 2);
  spr.deleteSprite();
}

void beginAlarmScreen() {
  tft.fillScreen(TFT_BLACK);
  msgPixW = efontWidth(MSG_ALARM, 2);
  scrollX = SCR_W;
  spr.setColorDepth(16);
  spr.createSprite(SCR_W, BAND_H);
}

void endAlarmScreen() {
  spr.deleteSprite();
  drawIdle();
}

void drawScroll() {
  spr.fillSprite(TFT_BLACK);
  drawEfontSpr(&spr, MSG_ALARM, scrollX, (BAND_H - 32) / 2, 2, TFT_YELLOW);
  spr.pushSprite(0, (SCR_H - BAND_H) / 2);
  scrollX -= SCROLL_STEP;
  if (scrollX < -msgPixW) scrollX = SCR_W;
}

// ---------- 서버 ----------
bool pollState() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  bool st = false;
  String url = String(SB_URL) + "/rest/v1/call_light?id=eq." + LIGHT_ID + "&select=state";
  if (!http.begin(client, url)) return false;
  http.addHeader("apikey", SB_KEY);
  http.addHeader("Authorization", String("Bearer ") + SB_KEY);
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok && doc.size() > 0)
      st = doc[0]["state"] | false;
  } else Serial.printf("[GET] fail %d RSSI %d\n", code, WiFi.RSSI());
  http.end();
  return st;
}

void clearState() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String(SB_URL) + "/rest/v1/call_light?id=eq." + LIGHT_ID;
  if (!http.begin(client, url)) return;
  http.addHeader("apikey", SB_KEY);
  http.addHeader("Authorization", String("Bearer ") + SB_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");
  http.setTimeout(5000);
  Serial.printf("[CLEAR] %d\n", http.sendRequest("PATCH", "{\"state\":false,\"expire_at\":null}"));
  http.end();
}

// ntfy 전송 (Title 은 ASCII 만)
void sendNtfy() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, NTFY_URL)) return;
  http.addHeader("Title", "QC CALL");
  http.addHeader("Priority", "high");
  http.addHeader("Tags", "bell");
  http.setTimeout(5000);
  int code = http.POST("수작업검사실 콜");
  Serial.printf("[NTFY] %d\n", code);
  http.end();

  // 결과 화면 + 확인음
  if (code >= 200 && code < 300) {
    drawCenterText("호출했습니다", TFT_GREEN);
    for (int i = 0; i < 2; i++) { buzz(true); delay(50); buzz(false); delay(60); }
  } else {
    drawCenterText("호출 실패", TFT_RED);
    buzz(true); delay(400); buzz(false);
  }
  msgUntil = millis() + 2000;   // 2초 후 대기중 복귀
}

// ---------- 알림 제어 ----------
void startAlarm() {
  alarmOn = true;
  alarmStart = millis();
  lastBlink = lastScroll = millis();
  patIdx = 0; patNext = millis();
  beginAlarmScreen();
  Serial.println("[ALARM] start");
}

void stopAlarm() {
  alarmOn = false;
  buzz(false);
  digitalWrite(LED_PIN, LOW);
  ledState = false;
  endAlarmScreen();
  Serial.println("[ALARM] end");
  if (WiFi.status() == WL_CONNECTED) clearState();
}

// 리듬 패턴 (non-blocking)
void runBuzzer() {
  if (millis() < patNext) return;
  buzz(patIdx % 2 == 0);
  patNext = millis() + PATTERN[patIdx];
  patIdx = (patIdx + 1) % PATTERN_N;
}

// ---------- 터치: 짧게=해제 / 길게(1초)=콜 ----------
void handleTouch() {
  static bool prevT = false;
  static unsigned long tPress = 0;
  static bool longFired = false;
  unsigned long now = millis();

  bool curT = (digitalRead(TOUCH_PIN) == HIGH);

  if (curT && !prevT) {                 // 눌림 시작
    tPress = now;
    longFired = false;
  }

  // 누른 채 1초 경과 → 길게 누름 확정 (떼기 전에 즉시 발동)
  if (curT && !longFired && now - tPress >= LONG_PRESS_MS) {
    longFired = true;
    Serial.println("[TOUCH] long -> stop + ntfy");
    buzz(true); delay(60); buzz(false);   // 인식 피드백
    if (alarmOn) stopAlarm();
    sendNtfy();
  }

  // 짧게 떼면 해제만
  if (!curT && prevT && !longFired) {
    Serial.println("[TOUCH] short -> stop");
    if (alarmOn) stopAlarm();
  }

  prevT = curT;
}

// ---------- 기본 ----------
void setup() {
  pinMode(TOUCH_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  ledcSetup(BUZZ_CH, BUZZ_PWM_HZ, BUZZ_RES);
  ledcAttachPin(BUZZ_PIN, BUZZ_CH);
  ledcWrite(BUZZ_CH, 0);

  Serial.begin(115200);
  delay(200);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  spr.setColorDepth(16);
  spr.createSprite(SCR_W, 32);
  spr.fillSprite(TFT_BLACK);
  drawEfontSpr(&spr, "WiFi 연결중", 8, 0, 2, TFT_WHITE);
  spr.pushSprite(0, (SCR_H - 32) / 2);
  spr.deleteSprite();

  wifiMulti.addAP("taejin-A", "taejinpress");
  wifiMulti.addAP("TAEJIN", "taejinpress");
  wifiMulti.addAP("U+NetC2B3", "4151007221");

  Serial.print("WiFi connecting");
  while (wifiMulti.run() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.printf("\nIP: %s  RSSI: %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  buzz(true); delay(60); buzz(false);   // 부팅 확인음
  drawIdle();
}

void loop() {
  if (wifiMulti.run() != WL_CONNECTED) { delay(500); }

  handleTouch();

  // 호출 결과 메시지 만료 → 대기중 복귀
  if (msgUntil && millis() > msgUntil) {
    msgUntil = 0;
    if (!alarmOn) drawIdle();
  }

  if (alarmOn) {
    if (millis() - alarmStart >= ALARM_MS) {
      stopAlarm();                        // 10초 자동 해제
    } else {
      runBuzzer();
      if (millis() - lastScroll >= SCROLL_MS) { lastScroll = millis(); drawScroll(); }
      if (millis() - lastBlink >= BLINK_MS) {
        lastBlink = millis();
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
      }
    }
  }
  else if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    if (WiFi.status() == WL_CONNECTED && pollState()) startAlarm();
  }
  delay(5);
}
