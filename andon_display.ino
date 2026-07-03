/*
  andon_display.ino  v3.13 (다중 메시지 2초 순환·녹색배경)
  태진다이텍 현장 안돈 디스플레이 (ESP32 + 2.25" ST7789 76x284 TFT-SPI (가로))

  동작
   - 부팅 시 자기 MAC(콜론제거 대문자 12자리)을 Supabase andon 테이블에 자동 등록(heartbeat)
   - twin_factory 에서 호기↔MAC 매핑 후 설비 선택 → WORST 메시지 발송
   - 5초마다 andon?mac=eq.{내MAC} 폴링 → 변경 시 화면 갱신
   - 30초마다 last_seen 갱신(heartbeat) → twin_factory 온라인 표시

  필요 라이브러리 (Arduino IDE 라이브러리 매니저)
   - GFX Library for Arduino  (moononournation)  : NV3007 142x428 2.79" 지원
   - U8g2                      (olikraus)         : 한글/큰숫자 폰트
   - ArduinoJson v6/v7         (bblanchon)

  보드/설정
   - ESP32 Dev Module / arduino-esp32 core 2.0.17  (core 3.x는 TLS 회귀 → 사용 금지)
   - Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"  ← 한글폰트 용량 때문에 필수

  배선 (ESP32 ↔ ST7789 8핀)  ※ 라벨 GND VCC SCL SDA RST DC CS BL
   GND -> GND (공통 필수)
   VCC -> 5V (또는 3V3)
   SCL -> GPIO18 (SCLK)
   SDA -> GPIO23 (MOSI)
   RST -> GPIO4
   DC  -> GPIO21
   CS  -> GPIO5
   BL  -> GPIO16 (백라이트 PWM 밝기조절. 이 모듈은 LOW=켜짐 반전형)
*/

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include "time.h"
#include "esp_mac.h"

// ===== 사용자 설정 =====
WiFiMulti wifiMulti;   // 신호 센 AP 자동 선택
struct AP { const char* ssid; const char* pass; };
AP APS[] = {
  {"TAEJIN",   "taejinpress"},
  {"taejin-A", "taejinpress"},
};

const char* SB_URL  = "https://omngtyewdaqpphnzeate.supabase.co";
const char* SB_KEY  = "sb_publishable_9j2YkkL-7ul1TrhH-NjVdQ_vWDG2-1D";

const char* HO_CODE = "37";   // ★ 이 디스플레이가 담당할 설비호기 (예 "37"). 보드마다 지정. 빈값이면 미지정

const unsigned long SYNC_MS = 30000;   // 동기 주기(폴링+heartbeat 통합). 더 줄이려면 60000(1분)

// ===== 핀 =====
#define LCD_DC   21
#define LCD_CS   5
#define LCD_RST  4
#define LCD_SCK  18
#define LCD_MOSI 23
#define LCD_MISO -1
#define LCD_BL   16        // BL은 GPIO16에 연결 (GND 직결 대신). 이 모듈은 LOW=켜짐(반전)

// ===== 부저 (액티브 KY-012) : S→GPIO17, 중간VCC→3.3V, -→GND =====
#define BUZZER_PIN 17
// 리듬: {소리ms, 쉼ms} 순서대로. 짧게-짧게-짧게-길게 (♪♪♪♩)
const int BEEP_ON[]  = {90, 90, 90, 350};
const int BEEP_OFF[] = {80, 80, 80, 250};
void buzzerBeep() {
  int n = sizeof(BEEP_ON) / sizeof(BEEP_ON[0]);
  for (int i = 0; i < n; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(BEEP_ON[i]);
    digitalWrite(BUZZER_PIN, LOW);  delay(BEEP_OFF[i]);
  }
}

// ===== 백라이트 (PWM, 반전: 듀티 낮을수록 밝음) =====
#define BL_FREQ    20000
#define BL_RES     8
int BL_BRIGHT = 40;        // 밝기 0~100 (낮을수록 어둡게 보고 싶으면 값↓). 40 권장
void blApply() {
  int duty = map(constrain(BL_BRIGHT, 0, 100), 0, 100, 255, 0);  // 반전: 100%→duty0(가장 밝음)
  ledcWrite(LCD_BL, duty);                                       // 코어 3.x: 핀 기준
}
// 새 알림 시 백라이트 5회 밝게 깜빡 (반전: duty0=최대밝기, 255=꺼짐)
void blBlink5() {
  for (int i = 0; i < 10; i++) {
    ledcWrite(LCD_BL, 0);   delay(120);
    ledcWrite(LCD_BL, 255); delay(120);
  }
  blApply();                // 원래 밝기 복원
}

// ===== 디스플레이 (NV3007 2.79" 142x428) =====
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
// ST7789P3 2.25" 76x284 (EStarDyn) — 가로 사용(rotation=1 → 284x76). 오프셋 82/18(가로시 자동 swap)
Arduino_GFX *gfx = new Arduino_ST7789(
  bus, LCD_RST, 1 /*rotation 가로*/, true /*IPS*/,
  76 /*native width*/, 284 /*native height*/,
  82 /*col_off1*/, 18 /*row_off1*/, 82 /*col_off2*/, 18 /*row_off2*/);

#define SCR_W 284
#define SCR_H 76

// 색 (RGB565)
#define C_BG    0x0000
#define C_TXT   0xFFFF
#define C_SUB   0x8410
#define C_GREEN 0x07E0
#define C_RED   0xF800
#define C_AMBER 0xFD20
#define C_GRAY  0x4208
#define C_BLUE  0x2D7F

// ===== 상태 =====
String MAC12, LABEL4;
String lastSig = "~init~";       // 변경감지 (updated_at+risk_level+msg)
bool   wifiOk = false;
unsigned long tSync = 0;

// 다중 메시지 (1시간 이내, 2초 순환)
#define MSG_MAX 20
#define MSG_ROTATE_MS 2000
String msgArr[MSG_MAX], msgAt[MSG_MAX];
long   msgIds[MSG_MAX];
int    msgN = 0, cycleIdx = 0;
long   lastMaxId = -1;
unsigned long tCycle = 0;
#define ALERT_BG 0x0320   // 짙은 녹색 배경 (RGB565)

// ---------- KST 시간 ----------
String kstNow() {
  struct tm t;
  if (!getLocalTime(&t, 200)) return "";
  char b[40];
  snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d+09:00",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return String(b);
}
String hhmm(const String& iso) {        // "....T13:24:..." -> "13:24"
  int p = iso.indexOf('T');
  if (p < 0 || (int)iso.length() < p + 6) return "";
  return iso.substring(p + 1, p + 6);
}

// ---------- 폰트 헬퍼 ----------
void fKor() { gfx->setFont(u8g2_font_unifont_t_korean2); }   // 한글 16px (korean2: 글리프 넓음)
void fBig() { gfx->setFont(u8g2_font_logisoso32_tn); }       // 숫자 32px (.포함)
int  txtW(const String& s) { int16_t x1, y1; uint16_t w, h; gfx->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h); return (int)w; }
int  korW(const String& s) { fKor(); return txtW(s); }
void korCenter(const String& s, int y, uint16_t col) {
  fKor(); gfx->setTextColor(col);
  int w = txtW(s);
  gfx->setCursor((SCR_W - w) / 2, y); gfx->print(s);
}
void korRight(const String& s, int xRight, int y, uint16_t col) {
  fKor(); gfx->setTextColor(col);
  int w = txtW(s);
  gfx->setCursor(xRight - w, y); gfx->print(s);
}
void korAt(const String& s, int x, int y, uint16_t col) {
  fKor(); gfx->setTextColor(col); gfx->setCursor(x, y); gfx->print(s);
}

uint16_t levelColor(const String& lv) {
  if (lv == "danger") return C_RED;
  if (lv == "warn")   return C_AMBER;
  if (lv == "normal") return C_GREEN;
  return C_GRAY;
}

// ---------- 화면들 ----------
#define BW_W 0xFFFF   // 흰
#define BW_B 0x0000   // 검

void drawCenterMsg(const String& title, const String& sub, uint16_t col) {
  gfx->fillScreen(BW_B);
  korCenter(title, 34, BW_W);
  if (sub.length()) korCenter(sub, 62, BW_W);
}

// 가로 284x76 레이아웃:
//  [라벨·호기]            [항목]
//  [   큰 측정값   ]   [상태 %]
void drawAndon(JsonObject o) {
  String ho   = o["ho_code"]   | "";
  String lv   = o["risk_level"]| "none";
  String item = o["worst_item"]| "";
  String val  = o["worst_val"] | "";
  String stt  = o["worst_state"]|"";
  int    pct  = o["worst_pct"] | -1;
  String upd  = o["updated_at"]| "";
  bool danger = (lv == "danger" || lv == "warn");

  gfx->fillScreen(BW_B);
  uint16_t fg = BW_W, bg = BW_B;

  // 위험이면 전체 반전(흰 배경 + 검정 글자)
  if (danger) { gfx->fillScreen(BW_W); fg = BW_B; bg = BW_W; }

  // 상단 좌: 라벨·호기
  korAt(LABEL4 + (ho.length() ? ("  " + ho + "호기") : "  미지정"), 4, 18, fg);
  // 상단 우: 항목명
  korRight(item.length() ? item : "정상", SCR_W - 4, 18, fg);

  // 구분선
  gfx->drawFastHLine(4, 26, SCR_W - 8, fg);

  // 하단 좌: 큰 측정값
  if (val.length()) {
    fBig(); gfx->setTextColor(fg);
    gfx->setCursor(6, 68); gfx->print(val);
  }
  // 하단 우: 상태 + %
  String st2 = stt;
  if (pct >= 0 && stt.length() && stt != "정상") st2 += " " + String(pct) + "%";
  if (st2.length()) korRight(st2, SCR_W - 4, 64, fg);
}

// 자유 알림 텍스트 — 짙은 녹색 배경, 흰 글자, 자동 줄바꿈 + (n/총) 순번
void drawAlert(const String& msg, const String& aat, int idx, int total) {
  gfx->fillScreen(ALERT_BG);
  uint16_t fg = C_TXT;                                  // 흰 글자
  korAt((String(HO_CODE).length() ? String(HO_CODE) + "호기" : LABEL4) + " 알림", 4, 16, fg);
  String rt = hhmm(aat);
  if (total > 1) rt += "  " + String(idx + 1) + "/" + String(total);
  if (rt.length()) korRight(rt, SCR_W - 4, 16, fg);
  gfx->drawFastHLine(4, 22, SCR_W - 8, fg);
  fKor(); gfx->setTextColor(fg);
  int x = 4, y = 44, lineH = 18, maxW = SCR_W - 8, lines = 0;
  String word = "", line = "";
  String s = msg + " ";
  for (unsigned i = 0; i < s.length(); ) {
    unsigned char c = s[i]; int n = 1;
    if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
    String ch = s.substring(i, i + n); i += n;
    if (ch == " " || ch == "\n") {
      String test = line.length() ? (line + word) : word;
      if (txtW(test) > maxW && line.length()) {
        gfx->setCursor(x, y); gfx->print(line); y += lineH; if (++lines >= 3) return;
        line = word + (ch == "\n" ? "" : " ");
      } else {
        line = test + (ch == "\n" ? "" : " ");
      }
      if (ch == "\n") { gfx->setCursor(x, y); gfx->print(line); y += lineH; if (++lines >= 3) return; line = ""; }
      word = "";
    } else {
      word += ch;
      if (txtW(line + word) > maxW) {
        if (line.length()) { gfx->setCursor(x, y); gfx->print(line); y += lineH; if (++lines >= 3) return; line = ""; }
      }
    }
  }
  if (line.length()) { gfx->setCursor(x, y); gfx->print(line); }
}

// ---------- Supabase 동기: POST upsert(merge) + representation 으로 수신까지 1요청 ----------
#define ALERT_TTL_SEC 3600        // 알림 표시 유지 시간(초). 1시간
long alertAgeSec(const String& aat) {     // alert_at("YYYY-MM-DDTHH:MM:SS+09:00") 이후 경과초. 판단불가 시 -1
  if (aat.length() < 19) return -1;
  struct tm t = {0};
  t.tm_year = aat.substring(0, 4).toInt() - 1900;
  t.tm_mon  = aat.substring(5, 7).toInt() - 1;
  t.tm_mday = aat.substring(8, 10).toInt();
  t.tm_hour = aat.substring(11, 13).toInt();
  t.tm_min  = aat.substring(14, 16).toInt();
  t.tm_sec  = aat.substring(17, 19).toInt();
  time_t at = mktime(&t);
  time_t now = time(nullptr);
  if (now < 100000) return -1;            // 시간 미동기
  return (long)(now - at);
}
// heartbeat: andon 테이블 last_seen 갱신 (온라인 표시용)
void sbBeat() {
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient https;
  String url = String(SB_URL) + "/rest/v1/andon?on_conflict=mac";
  if (!https.begin(cli, url)) return;
  https.addHeader("apikey", SB_KEY);
  https.addHeader("Authorization", String("Bearer ") + SB_KEY);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");
  String ls = kstNow();
  String body = "[{\"mac\":\"" + MAC12 + "\",\"label\":\"" + LABEL4 + "\"";
  if (strlen(HO_CODE)) body += ",\"ho_code\":\"" + String(HO_CODE) + "\"";
  body += ",\"last_seen\":\"" + ls + "\"}]";
  https.POST(body);
  https.end();
}

// 1시간 이내 메시지 목록 로드 → msgArr[]. 신규 도착 시 깜빡+부저
void loadMsgs() {
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient https;
  String url = String(SB_URL) + "/rest/v1/andon_msg?ho_code=eq." + String(HO_CODE) +
               "&order=created_at.desc&limit=20&select=id,msg,created_at";
  if (!https.begin(cli, url)) return;
  https.addHeader("apikey", SB_KEY);
  https.addHeader("Authorization", String("Bearer ") + SB_KEY);
  int code = https.GET();
  if (code != 200) { https.end(); return; }
  String payload = https.getString();
  https.end();

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, payload)) return;
  JsonArray arr = doc.as<JsonArray>();

  // 1시간 이내만, 오래된→최신 순으로 저장
  int n = 0; long maxId = -1;
  for (int i = arr.size() - 1; i >= 0 && n < MSG_MAX; i--) {   // desc이므로 역순=asc
    JsonObject o = arr[i];
    String at = o["created_at"] | "";
    long age = alertAgeSec(at);
    if (age >= 0 && age >= ALERT_TTL_SEC) continue;            // 1시간 경과 제외
    msgIds[n] = o["id"] | 0;
    msgArr[n] = String((const char*)(o["msg"] | ""));
    msgAt[n]  = at;
    if (msgIds[n] > maxId) maxId = msgIds[n];
    n++;
  }
  bool isNew = (maxId > lastMaxId);
  msgN = n;
  if (maxId >= 0) lastMaxId = maxId;

  if (msgN == 0) {
    if (lastSig != "~none~") { lastSig = "~none~"; drawCenterMsg("대기중", String(HO_CODE).length() ? (String(HO_CODE) + "호기") : "미지정", 0); }
    return;
  }
  if (isNew) {                     // 신규 메시지 도착 → 첫 페이지부터, 깜빡+부저
    cycleIdx = 0; tCycle = millis();
    drawAlert(msgArr[cycleIdx], msgAt[cycleIdx], cycleIdx, msgN);
    lastSig = "~msg~"; blBlink5(); buzzerBeep();
  }
}

void sbSync() {
  if (WiFi.status() != WL_CONNECTED) { if (lastSig != "~off~") { lastSig = "~off~"; drawCenterMsg("WiFi 끊김", "재접속 중", 0); } return; }
  sbBeat();
  loadMsgs();
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[andon] boot v3.13");

  // 백라이트 PWM (LOW=켜짐 모듈이라 반전 듀티) — 코어 3.x: ledcAttach(핀,주파수,비트)
  ledcAttach(LCD_BL, BL_FREQ, BL_RES);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  blApply();
  Serial.print("[andon] BL PWM bright="); Serial.println(BL_BRIGHT);

  if (!gfx->begin(8000000)) {        // 8MHz로 낮춰 안정화 (배선 길면 고속 실패)
    Serial.println("[andon] gfx->begin() 실패! → 배선(RST/DC/CS/SCK/MOSI)·전원 확인");
  } else {
    Serial.println("[andon] gfx->begin() OK");
  }

  gfx->invertDisplay(true);          // 이 패널은 반전 상태 → true로 바로잡음(검정=검정, 흰=흰)
  gfx->fillScreen(BW_W);             // 흰화면 1초 (밝기 확인)
  delay(1000);
  gfx->fillScreen(BW_B);
  gfx->setUTF8Print(true);

  uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);   // efuse에서 직접 (WiFi 시작 전에도 유효)
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  MAC12  = String(buf);
  LABEL4 = MAC12.substring(8);                  // 끝 4자리 (예 "D4E5")
  Serial.print("[andon] MAC="); Serial.print(MAC12); Serial.print("  LABEL="); Serial.println(LABEL4);

  drawCenterMsg("연결 중", LABEL4, 0);

  WiFi.mode(WIFI_STA);
  for (unsigned i = 0; i < sizeof(APS) / sizeof(APS[0]); i++) wifiMulti.addAP(APS[i].ssid, APS[i].pass);
  unsigned long t0 = millis();
  while (wifiMulti.run() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print("."); }
  Serial.println();
  wifiOk = (WiFi.status() == WL_CONNECTED);

  if (wifiOk) {
    Serial.print("[andon] WiFi OK  SSID="); Serial.print(WiFi.SSID());
    Serial.print("  IP="); Serial.println(WiFi.localIP());
    configTime(9 * 3600, 0, "pool.ntp.org", "time.google.com");
    struct tm t; getLocalTime(&t, 3000);        // NTP 동기 대기(최대 3s)
    sbSync();                                    // 첫 동기(등록+수신) → 대기중/알림 표시
  } else {
    Serial.println("[andon] WiFi 실패");
    drawCenterMsg("WiFi 실패", "재시도 중", 0);
  }
  tSync = millis();
}

unsigned long tWifi = 0;
void loop() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED && now - tWifi >= 10000) {  // 10초마다만 재시도
    tWifi = now; wifiMulti.run();
  }
  if (now - tSync >= SYNC_MS) { tSync = now; sbSync(); }

  // 메시지 2개 이상이면 2초마다 순환 표시
  if (msgN >= 2 && now - tCycle >= MSG_ROTATE_MS) {
    tCycle = now;
    cycleIdx = (cycleIdx + 1) % msgN;
    drawAlert(msgArr[cycleIdx], msgAt[cycleIdx], cycleIdx, msgN);
  } else if (msgN == 1 && lastSig != "~one~") {
    lastSig = "~one~"; cycleIdx = 0;
    drawAlert(msgArr[0], msgAt[0], 0, 1);
  }
  delay(50);
}
