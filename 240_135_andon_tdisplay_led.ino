/*
  andon_tdisplay_led.ino  v2.0 (idle image, small label, LED 20x on new msg only)
  T-Display 1.14 135x240, HO=49, LED GPIO17-680ohm-LED-GND
  Libs: Arduino_GFX, U8g2
*/

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include "time.h"
#include "esp_mac.h"
#include "240_135_sumukhwa_img.h"

WiFiMulti wifiMulti;
const char* AP_SSID[] = { "TAEJIN", "taejin-A", "U+NetC2B3" };
const char* AP_PASS[] = { "taejinpress", "taejinpress", "4151007221" };
#define AP_COUNT 3

const char* SB_KEY  = "sb_publishable_9j2YkkL-7ul1TrhH-NjVdQ_vWDG2-1D";
const char* SB_HOST = "omngtyewdaqpphnzeate.supabase.co";
const char* HO_CODE = "49";

const unsigned long SYNC_MS = 30000;
#define ALERT_TTL_SEC 3600
#define SCROLL_STEP 2
#define SCROLL_MS   20
#define LED_BLINK_MS 300

#define LCD_DC   16
#define LCD_CS   5
#define LCD_RST  23
#define LCD_SCK  18
#define LCD_MOSI 19
#define LCD_MISO -1
#define LCD_BL   4
#define LED_PIN  17

#define BL_FREQ 20000
#define BL_RES  8
int BL_BRIGHT = 70;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
Arduino_GFX *tft = new Arduino_ST7789(bus, LCD_RST, 1, true, 135, 240, 52, 40, 53, 40);
Arduino_Canvas *gfx = new Arduino_Canvas(240, 135, tft);

#define SCR_W 240
#define SCR_H 135
#define C_BG  0x0000
#define C_FG  0xFFE0
#define C_SUB 0xFFFF

String MAC12, LABEL4;
unsigned long tSync = 0;
long lastMaxId = -1;
String scrollText = "";
int   scrollW = 0;
int   scrollX = SCR_W;
int   msgCount = 0;
int   gHttpCode = 0;

WiFiClientSecure gCli;

// manual prototypes (ArduinoDroid preprocessor workaround)
void blApply();
void newMsgBlink();
String clReadLine(unsigned long toMs);
long hexVal(String h);
String httpsReq(String method, String pathq, String body);
String kstNow();
long ageSec(String iso);
void fKor();
int txtW(String s);
void drawFrame();
void sbBeat();
void loadMsgs();
void sbSync();

void blApply() { ledcWrite(LCD_BL, map(constrain(BL_BRIGHT, 0, 100), 0, 100, 0, 255)); }

void newMsgBlink() {
  for (int i = 0; i < 20; i++) {
    if (i < 10) ledcWrite(LCD_BL, 255);
    digitalWrite(LED_PIN, HIGH); delay(120);
    if (i < 10) ledcWrite(LCD_BL, 0);
    digitalWrite(LED_PIN, LOW);  delay(120);
  }
  blApply();
}

String clReadLine(unsigned long toMs) {
  String l = ""; unsigned long t0 = millis();
  while (millis() - t0 < toMs) {
    if (gCli.available()) {
      int ch = gCli.read();
      if (ch < 0) continue;
      if (ch == '\n') break;
      if (ch != '\r') l += (char)ch;
    } else {
      if (!gCli.connected() && !gCli.available()) break;
      delay(1);
    }
  }
  return l;
}

long hexVal(String h) {
  long v = 0;
  for (unsigned i = 0; i < h.length(); i++) {
    char c = h[i];
    if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
    else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
    else break;
  }
  return v;
}

String httpsReq(String method, String pathq, String body) {
  gHttpCode = 0;
  gCli.stop();
  gCli.setInsecure();
  if (!gCli.connect(SB_HOST, 443)) return "";
  String req = method + " " + pathq + " HTTP/1.1\r\n";
  req += "Host: " + String(SB_HOST) + "\r\n";
  req += "apikey: " + String(SB_KEY) + "\r\n";
  req += "Authorization: Bearer " + String(SB_KEY) + "\r\n";
  if (body.length()) {
    req += "Content-Type: application/json\r\n";
    req += "Prefer: resolution=merge-duplicates,return=minimal\r\n";
    req += "Content-Length: " + String(body.length()) + "\r\n";
  }
  req += "Connection: close\r\n\r\n";
  gCli.write((const uint8_t*)req.c_str(), req.length());
  if (body.length()) gCli.write((const uint8_t*)body.c_str(), body.length());

  String line = clReadLine(8000);
  int sp = line.indexOf(' ');
  if (sp > 0) gHttpCode = line.substring(sp + 1, sp + 4).toInt();
  bool chunked = false;
  while (true) {
    line = clReadLine(5000);
    if (line.indexOf("chunked") >= 0) chunked = true;
    if (line.length() == 0) break;
    if (!gCli.connected() && !gCli.available()) break;
  }
  String out = "";
  if (chunked) {
    while (true) {
      long sz = hexVal(clReadLine(5000));
      if (sz <= 0) break;
      unsigned long t0 = millis();
      while (sz > 0 && millis() - t0 < 8000) {
        if (gCli.available()) { int ch = gCli.read(); if (ch >= 0) { out += (char)ch; sz--; } }
        else if (!gCli.connected() && !gCli.available()) break;
        else delay(1);
      }
      clReadLine(2000);
    }
  } else {
    unsigned long t0 = millis();
    while ((gCli.connected() || gCli.available()) && millis() - t0 < 8000) {
      if (gCli.available()) { int ch = gCli.read(); if (ch >= 0) out += (char)ch; }
      else delay(1);
    }
  }
  gCli.stop();
  return out;
}

String kstNow() {
  struct tm t;
  if (!getLocalTime(&t, 200)) return "";
  char b[40];
  snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d+09:00",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return String(b);
}

long ageSec(String iso) {
  if (iso.length() < 19) return -1;
  struct tm t = {0};
  t.tm_year = iso.substring(0, 4).toInt() - 1900;
  t.tm_mon  = iso.substring(5, 7).toInt() - 1;
  t.tm_mday = iso.substring(8, 10).toInt();
  t.tm_hour = iso.substring(11, 13).toInt();
  t.tm_min  = iso.substring(14, 16).toInt();
  t.tm_sec  = iso.substring(17, 19).toInt();
  time_t at = mktime(&t);
  time_t nw = time(NULL);
  if (nw < 100000) return -1;
  return (long)(nw - at);
}

void fKor() { gfx->setFont(u8g2_font_unifont_t_korean2); }

int txtW(String s) { int16_t x1, y1; uint16_t w, h; gfx->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h); return (int)w; }

void drawFrame() {
  gfx->fillScreen(C_BG);
  fKor();
  if (msgCount == 0) {
    gfx->draw16bitRGBBitmap(0, 0, (uint16_t*)IMG_SUMUK, IMG_W, IMG_H);
    gfx->setTextSize(1);
    gfx->setTextColor(0x0000);
    String s = String(HO_CODE) + "\xED\x98\xB8\xEA\xB8\xB0 \xEB\x8C\x80\xEA\xB8\xB0\xEC\xA4\x91";
    gfx->setCursor(6, 16); gfx->print(s);
  } else {
    gfx->setTextSize(1);
    gfx->setTextColor(C_SUB);
    gfx->setCursor(4, 14); gfx->print(String(HO_CODE) + "\xED\x98\xB8\xEA\xB8\xB0");
    String c = String(msgCount) + "\xEA\xB1\xB4";
    gfx->setCursor(SCR_W - txtW(c) - 4, 14); gfx->print(c);
    gfx->drawFastHLine(0, 20, SCR_W, 0x4208);
    gfx->setTextSize(2);
    gfx->setTextColor(C_FG);
    gfx->setCursor(scrollX, 88);     gfx->print(scrollText);
    gfx->setCursor(scrollX + 1, 88); gfx->print(scrollText);
  }
  gfx->flush();
}

void sbBeat() {
  String body = "[{\"mac\":\"" + MAC12 + "\",\"label\":\"" + LABEL4 + "\",\"ho_code\":\"" + String(HO_CODE) + "\",\"last_seen\":\"" + kstNow() + "\"}]";
  httpsReq("POST", "/rest/v1/andon?on_conflict=mac", body);
}

void loadMsgs() {
  String payload = httpsReq("GET", "/rest/v1/andon_msg?ho_code=eq." + String(HO_CODE) +
               "&order=created_at.desc&limit=20&select=id,msg,created_at", "");
  if (gHttpCode != 200) return;
  if (!payload.length()) return;

  long ids[20]; String msgs[20]; String ats[20];
  int cnt = 0; int pos = 0;
  while (cnt < 20) {
    int ip = payload.indexOf("\"id\":", pos); if (ip < 0) break;
    long id = payload.substring(ip + 5, payload.indexOf(',', ip)).toInt();
    int mp = payload.indexOf("\"msg\":\"", ip); if (mp < 0) break;
    mp += 7;
    String msg = "";
    while (mp < (int)payload.length()) {
      char c = payload[mp];
      if (c == '\\' && mp + 1 < (int)payload.length()) {
        char nx = payload[mp + 1];
        if (nx == 'n') msg += '\n';
        else if (nx == '"') msg += '"';
        else if (nx == '\\') msg += '\\';
        else if (nx == 'u') { mp += 4; }
        mp += 2; continue;
      }
      if (c == '"') break;
      msg += c; mp++;
    }
    int ap = payload.indexOf("\"created_at\":\"", mp); if (ap < 0) break;
    ap += 14;
    int ae = payload.indexOf('"', ap);
    ids[cnt] = id; msgs[cnt] = msg; ats[cnt] = payload.substring(ap, ae);
    cnt++;
    pos = ae;
  }

  String joined = ""; int n = 0; long maxId = -1;
  for (int i = cnt - 1; i >= 0; i--) {
    long age = ageSec(ats[i]);
    if (age < 0) continue;
    if (age >= ALERT_TTL_SEC) continue;
    if (ids[i] > maxId) maxId = ids[i];
    String hm = ats[i].substring(11, 16);
    if (joined.length()) joined += "   >>   ";
    joined += "[" + hm + "] " + msgs[i];
    n++;
  }
  bool isNew = (maxId > lastMaxId);
  if (maxId >= 0) lastMaxId = maxId;

  msgCount = n;
  if (n == 0) { scrollText = ""; scrollW = 0; digitalWrite(LED_PIN, LOW); drawFrame(); return; }

  scrollText = joined;
  fKor(); gfx->setTextSize(2);
  scrollW = txtW(scrollText) + 2;
  if (isNew) { scrollX = SCR_W; drawFrame(); newMsgBlink(); }
}

void sbSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  sbBeat();
  loadMsgs();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[andonT-LED] boot v2.0");

  ledcAttach(LCD_BL, BL_FREQ, BL_RES);
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
  blApply();

  if (!gfx->begin()) Serial.println("[andonT] gfx begin FAIL");
  gfx->fillScreen(C_BG);
  gfx->setUTF8Print(true);
  gfx->setTextWrap(false);
  gfx->flush();

  uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  MAC12 = String(buf); LABEL4 = MAC12.substring(8);
  Serial.println("[andonT] MAC=" + MAC12);

  fKor(); gfx->setTextSize(1); gfx->setTextColor(C_SUB);
  gfx->setCursor(4, 70); gfx->print("\xEC\x97\xB0\xEA\xB2\xB0 \xEC\xA4\x91 " + LABEL4);
  gfx->flush();

  WiFi.mode(WIFI_STA);
  for (int i = 0; i < AP_COUNT; i++) wifiMulti.addAP(AP_SSID[i], AP_PASS[i]);
  unsigned long t0 = millis();
  while (wifiMulti.run() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print("."); }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[andonT] WiFi OK " + WiFi.SSID());
    configTime(9 * 3600, 0, "pool.ntp.org", "time.google.com");
    struct tm t; getLocalTime(&t, 3000);
    sbSync();
  }
  if (msgCount == 0) drawFrame();
  tSync = millis();
}

unsigned long tWifi = 0;
unsigned long tFrame = 0;
unsigned long tLed = 0;
bool ledOn = false;

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
