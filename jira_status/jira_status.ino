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

// 1/10 of the screen; no PSRAM needed.
static uint32_t drawBuf[TFT_HOR_RES * TFT_VER_RES / 10 / 2];

static uint32_t tick() { return millis(); }

void setup() {
  Serial.begin(115200);
  settings_load();
  model_init();

  lv_init();
  lv_tick_set_cb(tick);
  lv_display_t* disp = lv_tft_espi_create(TFT_HOR_RES, TFT_VER_RES, drawBuf, sizeof(drawBuf));
  lv_display_set_rotation(disp, SCREEN_ROTATION);
  ui_init();
  ui_apply_settings();

  ui_boot("Connecting to WiFi", "");
  WiFi.setHostname(HOSTNAME);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback([](WiFiManager*) {
    ui_boot("WiFi setup", "Join the WiFi \"" WIFI_SETUP_AP "\" on your phone and pick your network.");
  });
  if (!wm.autoConnect(WIFI_SETUP_AP)) {
    ui_boot("WiFi failed", "Restarting...");
    delay(3000);
    ESP.restart();
  }

  configTzTime(TZ_INFO, NTP_1, NTP_2);
  web_begin();
  fetch_begin();

  String ip = WiFi.localIP().toString();
  ui_boot("Connected", "Settings: http://" + ip + "  (or http://" HOSTNAME ".local)\nUser admin, password " + gCfg.webPass);
  uint32_t until = millis() + 6000;
  while (millis() < until) { lv_timer_handler(); web_loop(); delay(5); }
  ui_start_dashboard();
}

void loop() {
  lv_timer_handler();
  web_loop();
  ui_tick();
  delay(5);
}
