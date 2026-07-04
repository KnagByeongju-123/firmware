/*
  andon_tdisplay.ino  v1.2 (대기중 이미지 배경)
  태진다이텍 현장 안돈 — 작은 디스플레이 스크롤 버전

  - andon_msg 테이블에서 1시간 이내 메시지 로드 → 이어붙여 우→좌 스크롤
  - 신규 메시지: 백라이트 10회 깜빡 + 부저(옵션)
  - 대기중: "37호기 대기중" 고정 표시
  - 30초 동기(heartbeat + 메시지)

  보드: ESP32 Dev Module (T-Display), core 3.x, Partition: Huge APP (3MB)
  라이브러리: GFX Library for Arduino, U8g2, ArduinoJson

  T-Display 내장 배선(변경 불가): MOSI 19, SCLK 18, CS 5, DC 16, RST 23, BL 4(HIGH=ON)
  부저(옵션): S→GPIO17, VCC→3V3, -→GND
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
#include "tdisplay_img.h"   // 대기중 배경 (240x135 RGB565)

// ===== 사용자 설정 =====
WiFiMulti wifiMulti;
struct AP { const char* ssid; const char* pass; };
AP APS[] = {
  {"TAEJIN",   "taejinpress"},
  {"taejin-A", "taejinpress"},
};

const char* SB_URL  = "https://omngtyewdaqpphnzeate.supabase.co";
const char* SB_KEY  = "sb_publishable_9j2YkkL-7ul1TrhH-NjVdQ_vWDG2-1D";

const char* HO_CODE = "31";            // ★ 담당 설비호기

const unsigned long SYNC_MS = 30000;   // 동기 주기
#define ALERT_TTL_SEC 3600             // 메시지 1시간 유지
#define SCROLL_STEP 2                  // 스크롤 픽셀/프레임 (빠르게 3~4)
#define SCROLL_MS   20                 // 프레임 간격 ms

// ===== 핀 (T-Display 내장) =====
#define LCD_DC   16
#define LCD_CS   5
#define LCD_RST  23
#define LCD_SCK  18
#define LCD_MOSI 19
#define LCD_MISO -1
#define LCD_BL   4          // HIGH = 켜짐 (일반형)
#define BUZZER_PIN 17       // 부저(옵션)

// ===== 백라이트 =====
#define BL_FREQ 20000
#define BL_RES  8
int BL_BRIGHT = 70;         // 0~100
void blApply() { ledcWrite(LCD_BL, map(constrain(BL_BRIGHT,0,100),0,100,0,255)); }
void blBlink() {            // 신규 메시지: 10회 깜빡
  for (int i = 0; i < 10; i++) { ledcWrite(LCD_BL,255); delay(120); ledcWrite(LCD_BL,0); delay(120); }
  blApply();
}

// ===== 부저 =====
const int BEEP_ON[]  = {90, 90, 90, 350};
const int BEEP_OFF[] = {80, 80, 80, 250};
void buzzerBeep() {
  for (unsigned i = 0; i < sizeof(BEEP_ON)/sizeof(BEEP_ON[0]); i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(BEEP_ON[i]);
    digitalWrite(BUZZER_PIN, LOW);  delay(BEEP_OFF[i]);
  }
}

// ===== 디스플레이 (ST7789V 135x240 → 가로 240x135) =====
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
Arduino_GFX *tft = new Arduino_ST7789(bus, LCD_RST, 1, true, 135, 240, 52, 40, 53, 40);
Arduino_Canvas *gfx = new Arduino_Canvas(240, 135, tft);   // 오프스크린(스크롤 부드럽게)

#define SCR_W 240
#define SCR_H 135
#define C_BG  0x0000
#define C_FG  0xFFE0      // 노란 글자
#define C_SUB 0xFFFF

// ===== 상태 =====
String MAC12, LABEL4;
unsigned long tSync = 0;
long lastMaxId = -1;
String scrollText = "";    // 이어붙인 스크롤 문자열
int   scrollW = 0;         // 텍스트 픽셀 폭
int   scrollX = SCR_W;     // 현재 x
int   msgCount = 0;
bool  timeOk = false;

// ---------- KST ----------
String kstNow() {
  struct tm t;
  if (!getLocalTime(&t, 200)) return "";
  char b[40];
  snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d+09:00",
           t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return String(b);
}
long ageSec(const String& iso) {
  if (iso.length() < 19) return -1;
  struct tm t = {0};
  t.tm_year = iso.substring(0,4).toInt()-1900;
  t.tm_mon  = iso.substring(5,7).toInt()-1;
  t.tm_mday = iso.substring(8,10).toInt();
  t.tm_hour = iso.substring(11,13).toInt();
  t.tm_min  = iso.substring(14,16).toInt();
  t.tm_sec  = iso.substring(17,19).toInt();
  time_t at = mktime(&t), now = time(nullptr);
  if (now < 100000) return -1;
  return (long)(now - at);
}

// ---------- 폰트 ----------
void fKor() { gfx->setFont(u8g2_font_unifont_t_korean2); }
int txtW(const String& s) { int16_t x1,y1; uint16_t w,h; gfx->getTextBounds(s.c_str(),0,0,&x1,&y1,&w,&h); return (int)w; }

// 2배 확대 스크롤용: setTextSize(2) → 한글 32px
void drawFrame() {
  gfx->fillScreen(C_BG);
  fKor();
  if (msgCount == 0) {
    gfx->draw16bitRGBBitmap(0, 0, (uint16_t*)IMG_IDLE, IDLE_W, IDLE_H);
    gfx->setTextSize(1);
    gfx->setTextColor(0x0000);
    String s = String(HO_CODE) + "호기 대기중";
    gfx->setCursor(6, 16); gfx->print(s);
    gfx->setCursor(7, 16); gfx->print(s);   // 굵게
  } else {
    gfx->setTextSize(1);
    gfx->setTextColor(C_SUB);
    gfx->setCursor(4, 14); gfx->print(String(HO_CODE) + "호기");
    String c = String(msgCount) + "건";
    gfx->setCursor(SCR_W - txtW(c) - 4, 14); gfx->print(c);
    gfx->drawFastHLine(0, 20, SCR_W, 0x4208);
    // 본문: 32px 노란 굵은 스크롤
    gfx->setTextSize(2);
    gfx->setTextColor(C_FG);
    gfx->setCursor(scrollX, 88);     gfx->print(scrollText);
    gfx->setCursor(scrollX + 1, 88); gfx->print(scrollText);  // 굵게
  }
  gfx->flush();
}

// ---------- Supabase ----------
void sbBeat() {
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient https;
  if (!https.begin(cli, String(SB_URL) + "/rest/v1/andon?on_conflict=mac")) return;
  https.addHeader("apikey", SB_KEY);
  https.addHeader("Authorization", String("Bearer ") + SB_KEY);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");
  String body = "[{\"mac\":\"" + MAC12 + "\",\"label\":\"" + LABEL4 + "\",\"ho_code\":\"" + String(HO_CODE) + "\",\"last_seen\":\"" + kstNow() + "\"}]";
  https.POST(body);
  https.end();
}

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

  String joined = ""; int n = 0; long maxId = -1;
  for (int i = arr.size() - 1; i >= 0; i--) {          // 오래된→최신
    JsonObject o = arr[i];
    String at = o["created_at"] | "";
    long age = ageSec(at);
    if (age < 0 || age >= ALERT_TTL_SEC) continue;     // 1시간 경과·시간미동기 제외
    long id = o["id"] | 0;
    if (id > maxId) maxId = id;
    String hm = at.substring(11, 16);                  // HH:MM
    if (joined.length()) joined += "   ◆   ";
    joined += "[" + hm + "] " + String((const char*)(o["msg"] | ""));
    n++;
  }
  bool isNew = (maxId > lastMaxId);
  if (maxId >= 0) lastMaxId = maxId;

  msgCount = n;
  if (n == 0) { scrollText = ""; scrollW = 0; drawFrame(); return; }

  scrollText = joined;
  fKor(); gfx->setTextSize(2);
  scrollW = txtW(scrollText) + 2;
  if (isNew) { scrollX = SCR_W; drawFrame(); blBlink(); buzzerBeep(); }
}

void sbSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  sbBeat();
  loadMsgs();
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[andonT] boot v1.2");

  ledcAttach(LCD_BL, BL_FREQ, BL_RES);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  blApply();

  if (!gfx->begin(40000000)) Serial.println("[andonT] gfx begin FAIL");
  gfx->fillScreen(C_BG);
  gfx->setUTF8Print(true);
  gfx->setTextWrap(false);          // 스크롤 한 줄 유지(줄바꿈 금지)
  gfx->flush();

  uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
  MAC12 = String(buf); LABEL4 = MAC12.substring(8);
  Serial.println("[andonT] MAC=" + MAC12);

  fKor(); gfx->setTextSize(1); gfx->setTextColor(C_SUB);
  gfx->setCursor(4, 70); gfx->print("연결 중 " + LABEL4);
  gfx->flush();

  WiFi.mode(WIFI_STA);
  for (unsigned i = 0; i < sizeof(APS)/sizeof(APS[0]); i++) wifiMulti.addAP(APS[i].ssid, APS[i].pass);
  unsigned long t0 = millis();
  while (wifiMulti.run() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print("."); }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[andonT] WiFi OK " + WiFi.SSID());
    configTime(9*3600, 0, "pool.ntp.org", "time.google.com");
    struct tm t; getLocalTime(&t, 3000);
    sbSync();
  } else {
    gfx->fillScreen(C_BG);
    gfx->setCursor(4, 70); gfx->print("WiFi 실패 재시도중");
    gfx->flush();
  }
  if (msgCount == 0) drawFrame();
  tSync = millis();
}

unsigned long tWifi = 0, tFrame = 0;
void loop() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED && now - tWifi >= 10000) { tWifi = now; wifiMulti.run(); }
  if (now - tSync >= SYNC_MS) { tSync = now; sbSync(); }

  if (msgCount > 0 && now - tFrame >= SCROLL_MS) {
    tFrame = now;
    scrollX -= SCROLL_STEP;
    if (scrollX < -scrollW) scrollX = SCR_W;
    drawFrame();
  }
  delay(5);
}
