/*
 * 새 보드 MAC 확인 전용  v1.0
 *  - WiFi/Supabase 연결 안 함 → env_current에 UNREG 유령행 안 생김
 *  - 정식 펌웨어와 동일한 방식(esp_read_mac, STA)으로 읽음 → 그대로 REGISTRY에 복사
 *  - 표시: T-Display 화면 + 시리얼모니터(115200)
 *
 *  사용법
 *   1) 이 스케치를 새 보드에 업로드 (속도 115200, 보드 ESP32 Dev Module)
 *   2) 화면 또는 시리얼모니터에 뜬 MAC을 확인
 *   3) 그 MAC을 등록 요청 → 정식 펌웨어 REGISTRY에 추가 후 다시 업로드
 */
#include <TFT_eSPI.h>
#include "esp_mac.h"

TFT_eSPI tft = TFT_eSPI();

String readMacSTA() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);          // efuse에서 직접 (정식 펌웨어와 동일)
  char b[18];
  sprintf(b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  String mac = readMacSTA();

  Serial.println();
  Serial.println("==================");
  Serial.println("  새 보드 MAC");
  Serial.println("  " + mac);
  Serial.println("==================");
  Serial.println("REGISTRY 추가 예:");
  Serial.printf("  {\"%s\", \"SHT41_n\", \"구역\"},\n", mac.c_str());

  tft.init();
  tft.setRotation(0);                         // 세로
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  int cx = tft.width() / 2;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("NEW BOARD MAC", cx, 36, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(mac.substring(0, 8), cx, 108, 4);   // 5C:01:3B
  tft.drawString(mac.substring(9),    cx, 148, 4);   // 07:99:28

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Serial 115200", cx, 212, 2);
}

void loop() {
  // MAC 표시만 — 반복 작업 없음
}
