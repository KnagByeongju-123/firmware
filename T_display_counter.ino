/*
  T-Display 무접점(24V SSR) 카운터  —  GPIO25 신호 입력
  라이브러리: TFT_eSPI, U8g2_for_TFT_eSPI, WiFi/WiFiMulti/HTTPClient/Preferences(내장)
  ESP32 core 2.0.17 / TFT_eSPI User_Setup = LILYGO T-Display
  저장: Supabase ETC 프로젝트 (구글시트 미사용)

  [Supabase ETC 테이블 DDL]
  -- 일별 누적(설정번호별, upsert)
  create table counter_daily(
    device_id  text        not null,
    count_date date        not null,
    daily_count int        not null default 0,
    updated_at timestamptz default now(),
    primary key(device_id, count_date)
  );
  alter table counter_daily disable row level security;
  grant all on counter_daily to anon, authenticated;

  -- 60초 단위 로그(시계열)
  create table counter_log(
    id         bigserial   primary key,
    device_id  text        not null,
    count_date date        not null,
    daily_count int        not null,
    log_time   timestamptz default now()
  );
  alter table counter_log disable row level security;
  grant all on counter_log to anon, authenticated;
*/

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <Preferences.h>
#include <time.h>

// ==================== 설정 ====================
const char* DEVICE_ID = "1공장_생산_1호기";      // 설정 고유번호(설정번호)

const int   SIGNAL_PIN  = 25;                     // GPIO25
const bool  ACTIVE_LOW  = true;                   // SSR가 GND로 당기면 true
const unsigned long DEBOUNCE_MS = 30;             // 채터링 제거(ms)

const char* SUPABASE_URL   = "https://jgvikmakenpllwxwdugk.supabase.co";
const char* SUPABASE_KEY   = "sb_publishable_sKp-6nz2PQ9LxQ5pF-nYkg_YwoEJN6S";
const char* TABLE_DAILY    = "counter_daily";     // 일별 누적(upsert)
const char* TABLE_LOG      = "counter_log";       // 60초 로그

const unsigned long PUSH_INTERVAL = 60000;        // 60초 업로드
// =============================================

WiFiMulti      wifiMulti;
TFT_eSPI       tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;
Preferences    prefs;

// 카운터 상태
long   dailyCount   = 0;
long   totalCount   = 0;      // 누적 총계(자정에도 리셋 안 됨)
int64_t lastResetSeq = 0;     // 마감처리 리셋 명령 시퀀스
String currentDate  = "";

// 입력 디바운스
int  lastReading  = -1;
int  stableState  = -1;
unsigned long lastDebounce = 0;

// 타이머
unsigned long lastPush   = 0;
long   lastPushedCount = 0;   // 마지막 전송 시점의 카운트
unsigned long lastNvs    = 0;
unsigned long lastUiTick = 0;
long   lastDrawnCount = -999999;
bool   lastWifiUp = false;
bool   pushOk = false;
String lastPushClock = "--:--";

const int IDLE_LEVEL = ACTIVE_LOW ? HIGH : LOW;   // 대기 레벨
const int ACT_LEVEL  = ACTIVE_LOW ? LOW  : HIGH;  // 활성 레벨

// ---------- 시간 ----------
bool timeReady() {
  time_t now = time(nullptr);
  return now > 1700000000;   // NTP 동기화 여부
}
String dateStr() {
  if (!timeReady()) return "";
  time_t n = time(nullptr); struct tm t; localtime_r(&n, &t);
  char b[11]; strftime(b, sizeof(b), "%Y-%m-%d", &t);
  return String(b);
}
String clockStr() {
  if (!timeReady()) return "--:--:--";
  time_t n = time(nullptr); struct tm t; localtime_r(&n, &t);
  char b[9]; strftime(b, sizeof(b), "%H:%M:%S", &t);
  return String(b);
}

// ---------- NVS ----------
void saveNvs() {
  prefs.putLong("cnt", dailyCount);
  prefs.putLong("tot", totalCount);
  prefs.putString("date", currentDate);
}
void loadNvs() {
  dailyCount   = prefs.getLong("cnt", 0);
  totalCount   = prefs.getLong("tot", 0);
  lastResetSeq = prefs.getLong64("rseq", 0);
  currentDate  = prefs.getString("date", "");
}

// ---------- 업로드 ----------
bool supabasePost(const char* table, const String& body, bool merge) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[POST] WiFi X"); return false; }
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient https;
  String url = String(SUPABASE_URL) + "/rest/v1/" + table;
  if (merge) url += "?on_conflict=device_id,count_date";
  if (!https.begin(cli, url)) { Serial.println("[POST] begin fail"); return false; }
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", merge ? "resolution=merge-duplicates" : "return=minimal");
  int code = https.POST(body);
  String resp = https.getString();
  Serial.printf("[POST] %s  code=%d  resp=%s\n", table, code, resp.c_str());
  https.end();
  return (code == 200 || code == 201 || code == 204);
}

void pushData(const String& d, long dcnt, long tcnt) {
  String body = "{\"device_id\":\"" + String(DEVICE_ID) +
                "\",\"count_date\":\"" + d +
                "\",\"daily_count\":" + String(dcnt) +
                ",\"total_count\":" + String(tcnt) + "}";
  bool a = supabasePost(TABLE_DAILY, body, true);   // 일별 upsert
  bool b = supabasePost(TABLE_LOG,   body, false);  // 60초 로그 insert
  pushOk = a && b;
  lastPushClock = clockStr().substring(0, 5);
}

// URL 인코딩(한글 device_id 쿼리용)
String urlEncode(const String& s) {
  String out; char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') out += (char)c;
    else { sprintf(buf, "%%%02X", c); out += buf; }
  }
  return out;
}

// 마감처리 리셋 명령 폴링
void pollCommand() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient https;
  String url = String(SUPABASE_URL) + "/rest/v1/counter_command?device_id=eq." +
               urlEncode(DEVICE_ID) + "&select=reset_seq";
  if (!https.begin(cli, url)) return;
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  int code = https.GET();
  if (code == 200) {
    String r = https.getString();
    int i = r.indexOf("reset_seq");
    if (i >= 0) { i = r.indexOf(':', i);
      if (i >= 0) {
        int64_t seq = atoll(r.c_str() + i + 1);
        if (seq > lastResetSeq) {
          lastResetSeq = seq;
          dailyCount = 0;                       // 당일만 리셋(누적 유지)
          lastDrawnCount = -999999;
          prefs.putLong64("rseq", lastResetSeq);
          saveNvs();
          https.end();
          if (currentDate != "") {              // 즉시 0 전송
            pushData(currentDate, dailyCount, totalCount);
            if (pushOk) lastPushedCount = dailyCount;
          }
          return;
        }
      }
    }
  }
  https.end();
}

// ---------- 디스플레이 ----------
void drawLabel() {
  tft.fillRect(0, 0, 205, 20, TFT_BLACK);
  u8f.setFont(u8g2_font_unifont_t_korean2);
  u8f.setForegroundColor(TFT_CYAN);
  u8f.setBackgroundColor(TFT_BLACK);
  u8f.setCursor(4, 16);
  u8f.print(DEVICE_ID);
}

void drawWifi() {
  bool up = (WiFi.status() == WL_CONNECTED);
  int level = 0;
  if (up) {
    long r = WiFi.RSSI();
    level = (r > -60) ? 4 : (r > -70) ? 3 : (r > -80) ? 2 : 1;
  }
  uint16_t col = up ? TFT_GREEN : TFT_RED;
  int bx = 210, by = 20, bw = 5, gap = 2;
  int h[4] = {4, 7, 10, 13};
  tft.fillRect(bx - 2, 2, 40, 20, TFT_BLACK);
  for (int i = 0; i < 4; i++) {
    int x = bx + i * (bw + gap);
    int hh = h[i];
    uint16_t c = (up && (i < level)) ? col : (up ? TFT_DARKGREY : TFT_RED);
    if (!up && i >= 1) c = TFT_DARKGREY;
    if (!up && i == 0) c = TFT_RED;
    tft.fillRect(x, by - hh, bw, hh, c);
  }
}

void drawCount() {
  tft.fillRect(0, 26, 240, 74, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawNumber(dailyCount, 120, 64, 7);   // 7-세그먼트 대형 폰트
}

void drawFooter() {
  tft.fillRect(0, 118, 240, 17, TFT_BLACK);
  u8f.setFont(u8g2_font_6x12_tf);
  u8f.setForegroundColor(pushOk ? TFT_GREEN : TFT_ORANGE);
  u8f.setBackgroundColor(TFT_BLACK);
  u8f.setCursor(4, 131);
  String rssi = (WiFi.status() == WL_CONNECTED) ? String(WiFi.RSSI()) : "--";
  u8f.print("UP " + lastPushClock + "  RSSI " + rssi + "  " + clockStr());
}

// ---------- 카운트 ----------
void handleSignal() {
  int reading = digitalRead(SIGNAL_PIN);
  if (reading != lastReading) { lastDebounce = millis(); lastReading = reading; }
  if (millis() - lastDebounce > DEBOUNCE_MS) {
    if (reading != stableState) {
      stableState = reading;
      if (stableState == ACT_LEVEL) {     // 활성 에지 = 1카운트
        dailyCount++;
        totalCount++;
      }
    }
  }
}

// ---------- 날짜 전환 ----------
void checkRollover() {
  if (!timeReady()) return;
  String today = dateStr();
  if (currentDate == "") { currentDate = today; saveNvs(); return; }
  if (today != currentDate) {
    pushData(currentDate, dailyCount, totalCount);   // 전일 마감 전송
    currentDate = today;
    dailyCount = 0;                                   // 당일만 리셋
    lastPushedCount = 0;
    lastDrawnCount = -999999;
    saveNvs();
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(SIGNAL_PIN, ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  lastReading = digitalRead(SIGNAL_PIN);
  stableState = lastReading;

  pinMode(4, OUTPUT);          // T-Display 백라이트
  digitalWrite(4, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("BOOT", 120, 67, 4);   // 부팅 확인용 (여기까지 나오면 화면 정상)
  delay(800);
  tft.fillScreen(TFT_BLACK);
  u8f.begin(tft);

  prefs.begin("counter", false);
  loadNvs();
  lastPushedCount = dailyCount;

  drawLabel();
  drawCount();

  WiFi.mode(WIFI_STA);
  wifiMulti.addAP("Android");                 // 개방(비번없음)
  wifiMulti.addAP("TAEJIN",   "taejinpress");
  wifiMulti.addAP("taejin-A", "taejinpress");
  wifiMulti.run(8000);

  configTime(9 * 3600, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");

  // NTP 최대 6초 대기 후 즉시 테스트 전송(디버그)
  for (int i = 0; i < 60 && !timeReady(); i++) delay(100);
  Serial.printf("[BOOT] wifi=%d time=%d date=%s\n",
                WiFi.status() == WL_CONNECTED, timeReady(), dateStr().c_str());
  if (timeReady()) {
    currentDate = dateStr();
    pushData(currentDate, dailyCount, totalCount);   // 테스트 1회
    if (pushOk) lastPushedCount = dailyCount;
  }

  lastPush = millis();
}

void loop() {
  handleSignal();

  static unsigned long lastWifiRun = 0;
  if (millis() - lastWifiRun > 5000) {
    wifiMulti.run();
    lastWifiRun = millis();
  }

  // 마감처리 리셋 명령 확인(5초)
  static unsigned long lastCmd = 0;
  if (millis() - lastCmd > 5000) {
    pollCommand();
    lastCmd = millis();
  }

  checkRollover();

  // NVS 저장(5초, 변화시)
  if (millis() - lastNvs > 5000) {
    static long savedSnap = -1;
    if (dailyCount != savedSnap) { saveNvs(); savedSnap = dailyCount; }
    lastNvs = millis();
  }

  // 60초 업로드 — 카운트가 증가했을 때만, 성공 시에만 확정
  if (millis() - lastPush > PUSH_INTERVAL) {
    if (currentDate != "" && dailyCount != lastPushedCount) {
      pushData(currentDate, dailyCount, totalCount);
      if (pushOk) lastPushedCount = dailyCount;   // 실패시 다음 주기 재시도
    }
    lastPush = millis();
  }

  // UI 갱신
  if (dailyCount != lastDrawnCount) { drawCount(); lastDrawnCount = dailyCount; }
  if (millis() - lastUiTick > 1000) {
    drawWifi();
    drawFooter();
    bool up = (WiFi.status() == WL_CONNECTED);
    if (up != lastWifiUp) { lastWifiUp = up; }
    lastUiTick = millis();
  }
}
