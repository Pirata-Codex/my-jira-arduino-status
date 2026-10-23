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

// Saved normal WiFi (from the setup portal). Returns true when connected.
static bool tryNormal(WiFiManager& wm) {
  if (!wm.getWiFiIsSaved()) return false;
  ui_boot("Connecting to WiFi", wm.getWiFiSSID(true));
  LOG("normal wifi: %s", wm.getWiFiSSID(true).c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  uint32_t until = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < until) { lv_timer_handler(); delay(100); }
  if (WiFi.status() == WL_CONNECTED) return true;
  LOG("normal wifi failed");
  WiFi.disconnect(false, false);  // keep the saved credentials
  delay(200);
  return false;
}

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
  WiFi.persistent(false);  // don't overwrite the saved normal WiFi with the company SSID
  const char* identity = gCfg.entIdentity.length() ? gCfg.entIdentity.c_str() : gCfg.entUser.c_str();
  WiFi.begin(gCfg.entSsid.c_str(), WPA2_AUTH_PEAP, identity, gCfg.entUser.c_str(), gCfg.entPass.c_str(),
             gCfg.entCa.length() ? gCfg.entCa.c_str() : nullptr);
  uint32_t until = millis() + 25000;
  while (WiFi.status() != WL_CONNECTED && millis() < until) { lv_timer_handler(); delay(100); }
  if (WiFi.status() == WL_CONNECTED) return true;
  LOG("enterprise wifi failed, falling back to normal wifi");
  WiFi.disconnect(false, false);
  WiFi.persistent(true);
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
  WiFiManager wm;
  // Order: 1) saved normal WiFi  2) company WiFi (PEAP)  3) own hotspot with the setup portal.
  bool online = tryNormal(wm) || tryEnterprise();

  if (!online) {
    wm.setConfigPortalTimeout(300);
    // Company WiFi fields in the portal: pick the company network, leave its normal password
    // empty and fill these; the board saves them and restarts.
    WiFiManagerParameter pHead("<p><b>Company WiFi (WPA2/WPA3-Enterprise, PEAP)</b><br>"
                               "<small>For the company network: pick it above, leave its password empty, fill these.</small></p>");
    WiFiManagerParameter pUser("entUser", "Username (as on your laptop)", gCfg.entUser.c_str(), 64);
    WiFiManagerParameter pPass("entPass", "Password", "", 64, "type='password'");
    wm.addParameter(&pHead);
    wm.addParameter(&pUser);
    wm.addParameter(&pPass);
    static bool portalSaved = false;
    wm.setBreakAfterConfig(true);  // run the save callback even when the normal connect fails
    wm.setSaveConfigCallback([] { portalSaved = true; });
    ui_boot("WiFi setup", "Join the WiFi \"" WIFI_SETUP_AP "\" on your phone\nand open http://192.168.4.1");
    LOG("no wifi, starting setup hotspot %s", WIFI_SETUP_AP);
    online = wm.startConfigPortal(WIFI_SETUP_AP);

    if (portalSaved && strlen(pUser.getValue()) && strlen(pPass.getValue())) {
      // The network picked in the portal is the company SSID; WiFiManager's own connect can't do PEAP.
      gCfg.entSsid = wm.getWiFiSSID(true);
      gCfg.entUser = pUser.getValue();
      gCfg.entIdentity = "";
      gCfg.entPass = pPass.getValue();
      settings_save();
      wm.resetSettings();  // the "normal" slot now holds the company SSID without a password; clear it
      LOG("company wifi saved from portal: %s", gCfg.entSsid.c_str());
      ui_boot("Company WiFi saved", "Restarting to connect...");
      delay(1500);
      ESP.restart();
    }
    if (!online) {
      ui_boot("WiFi failed", "Restarting...");
      delay(3000);
      ESP.restart();
    }
  }

  LOG("wifi ok, ip %s", WiFi.localIP().toString().c_str());
  configTzTime(TZ_INFO, NTP_1, NTP_2);
  web_begin();

  String ip = WiFi.localIP().toString();
  ui_boot("Connected", "Settings: http://" + ip + "  (or http://" HOSTNAME ".local)\nUser admin, password " + gCfg.webPass);
  uint32_t until = millis() + 6000;
  // Show the address, and wait for the clock (NTP) before the first refresh.
  while (millis() < until || (time(nullptr) < 1700000000 && millis() < until + 15000)) {
    lv_timer_handler();
    web_loop();
    delay(5);
  }
  ui_refresh();
}

void loop() {
  lv_timer_handler();
  ui_tick();
  web_loop();
  if (fetch_due()) ui_refresh();
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    LOG("alive, wifi %d", (int)WiFi.status());
  }
  delay(5);
}
