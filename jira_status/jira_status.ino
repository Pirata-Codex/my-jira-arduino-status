// Jira status board: ESP32 + 320x480 TFT (landscape), LVGL 9 on TFT_eSPI.
// Talks to Jira, Uptime Kuma and a calendar directly over WiFi; no PC needed.

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "config.h"
#include "model.h"
#include "settings.h"
#include "fetch.h"
#include "web.h"
#include "ui.h"

#if !LV_USE_TFT_ESPI || !LV_USE_CHART || !LV_FONT_MONTSERRAT_48
#error "LVGL is using another lv_conf.h. Copy extras/lv_conf.h over Documents/Arduino/libraries/lv_conf.h"
#endif

// 1/20 of the screen, allocated at runtime (a static buffer doesn't fit the DRAM segment).
static const size_t DRAW_BUF_BYTES = TFT_HOR_RES * TFT_VER_RES / 20 * 2;

static uint32_t tick() { return millis(); }

// LVGL warnings and errors (e.g. "malloc failed") go to the serial monitor.
static void lvLog(lv_log_level_t level, const char* msg) { Serial.print("[lvgl] "); Serial.print(msg); }

// Company WiFi (WPA2/WPA3-Enterprise, PEAP). Returns true when connected.
static bool tryEnterprise() {
  if (gCfg.entSsid.isEmpty() || gCfg.entUser.isEmpty()) return false;
  ui_boot("Connecting to company WiFi", gCfg.entSsid);
  LOG("enterprise wifi: %s as %s (CA %s)", gCfg.entSsid.c_str(), gCfg.entUser.c_str(), gCfg.entCa.length() ? "set" : "none");
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    // Reason codes: 2 AUTH_EXPIRE, 15 4WAY_HANDSHAKE_TIMEOUT, 23 802_1X_AUTH_FAILED, 201 NO_AP_FOUND, 202 AUTH_FAIL
    LOG("wifi disconnected, reason %d", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  const char* identity = gCfg.entIdentity.length() ? gCfg.entIdentity.c_str() : gCfg.entUser.c_str();
  WiFi.begin(gCfg.entSsid.c_str(), WPA2_AUTH_PEAP, identity, gCfg.entUser.c_str(), gCfg.entPass.c_str(),
             gCfg.entCa.length() ? gCfg.entCa.c_str() : nullptr);
  uint32_t until = millis() + 25000;
  while (WiFi.status() != WL_CONNECTED && millis() < until) { lv_timer_handler(); delay(100); }
  if (WiFi.status() == WL_CONNECTED) return true;
  LOG("enterprise wifi failed, falling back to normal wifi");
  WiFi.disconnect(true);
  delay(200);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  LOG("boot, reset reason %d", (int)esp_reset_reason());
  settings_load();
  model_init();

  lv_init();
  lv_tick_set_cb(tick);
  lv_log_register_print_cb(lvLog);
  void* drawBuf = heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  lv_display_t* disp = lv_tft_espi_create(TFT_HOR_RES, TFT_VER_RES, drawBuf, DRAW_BUF_BYTES);
  lv_display_set_rotation(disp, SCREEN_ROTATION);
  ui_init();
  ui_apply_settings();

  ui_boot("Connecting to WiFi", "");
  WiFi.setHostname(HOSTNAME);
  bool online = tryEnterprise();
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback([](WiFiManager*) {
    ui_boot("WiFi setup", "Join the WiFi \"" WIFI_SETUP_AP "\" on your phone and pick your network.");
  });
  if (!online && !wm.autoConnect(WIFI_SETUP_AP)) {
    ui_boot("WiFi failed", "Restarting...");
    delay(3000);
    ESP.restart();
  }

  LOG("wifi ok, ip %s", WiFi.localIP().toString().c_str());
  configTzTime(TZ_INFO, NTP_1, NTP_2);
  web_begin();
  fetch_begin();

  String ip = WiFi.localIP().toString();
  ui_boot("Connected", "Settings: http://" + ip + "  (or http://" HOSTNAME ".local)\nUser admin, password " + gCfg.webPass);
  uint32_t until = millis() + 6000;
  while (millis() < until) { lv_timer_handler(); web_loop(); delay(5); }
  ui_start_dashboard();
  LOG("dashboard started");
}

void loop() {
  lv_timer_handler();
  web_loop();
  ui_tick();
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    LOG("alive, wifi %d", (int)WiFi.status());
  }
  delay(5);
}
