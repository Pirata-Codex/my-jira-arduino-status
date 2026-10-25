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

// 1/10 of the screen, allocated at runtime (a static buffer doesn't fit the DRAM segment).
// Fewer, larger SPI pushes; RAM allows it since the dashboard is freed during HTTPS refreshes.
// 1/20 of the screen (15 KB): drawing takes a few more passes, TLS gets 15 KB more.
static const size_t DRAW_BUF_BYTES = TFT_HOR_RES * TFT_VER_RES / 20 * 2;
void lvgl_pool_stats(unsigned& usedKb, unsigned& peakKb, unsigned& overflow);

// HTTPS (TLS handshake + JSON) runs in loop() now; the default 8 KB loop stack overflowed.
SET_LOOP_TASK_STACK_SIZE(20 * 1024);

static uint32_t tick() { return millis(); }

// LVGL warnings and errors (e.g. "malloc failed") go to the serial monitor.
static void lvLog(lv_log_level_t level, const char* msg) { Serial.print("[lvgl] "); Serial.print(msg); }

// Saved normal WiFi (from the setup portal). Returns true when connected.
// The saved WiFi is tried twice for 20 s each: a router that is slow to answer (or still booting
// after a power cut) used to send the board to the setup hotspot, which looked like it had
// forgotten the password. The saved credentials are never erased here.
static bool tryNormal(WiFiManager& wm) {
  if (!wm.getWiFiIsSaved()) return false;
  String ssid = wm.getWiFiSSID(true);
  WiFi.mode(WIFI_STA);
  for (int attempt = 1; attempt <= 2; attempt++) {
    ui_boot("Connecting to WiFi", ssid + (attempt > 1 ? " (retry)" : ""));
    LOG("normal wifi: %s, attempt %d", ssid.c_str(), attempt);
    WiFi.begin();
    uint32_t until = millis() + 20000;
    while (WiFi.status() != WL_CONNECTED && millis() < until) { lv_timer_handler(); delay(100); }
    if (WiFi.status() == WL_CONNECTED) return true;
    LOG("normal wifi failed (status %d)", (int)WiFi.status());
    WiFi.disconnect(false, false);  // keep the saved credentials
    delay(500);
  }
  return false;
}

// Company WiFi (WPA2/WPA3-Enterprise, PEAP). Returns true when connected.
static bool tryEnterprise() {
  if (gCfg.entSsid.isEmpty() || gCfg.entUser.isEmpty()) return false;
  ui_boot("Connecting to company WiFi", gCfg.entSsid);
  LOG("enterprise wifi: %s as %s (CA %s)", gCfg.entSsid.c_str(), gCfg.entUser.c_str(), gCfg.entCa.length() ? "set" : "none");
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

// ---------- screenshot ----------
// The display can't be read back (no MISO pin), so a screenshot redraws the whole screen once and
// sends each strip LVGL renders to the browser as well as to the display: no extra RAM.
static WiFiClient* shotClient = nullptr;
static int shotRow = 0;

// Same as the lv_tft_espi driver's flush (its driver data starts with the TFT_eSPI pointer),
// plus the copy for a screenshot in progress.
static void flushWithCapture(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  TFT_eSPI* tft = *(TFT_eSPI**)lv_display_get_driver_data(disp);
  uint32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  tft->startWrite();
  tft->setAddrWindow(area->x1, area->y1, w, h);
  tft->pushColors((uint16_t*)px, w * h, true);
  tft->endWrite();
  if (shotClient && area->x1 == 0 && (int)w == lv_display_get_horizontal_resolution(disp) && area->y1 == shotRow) {
    shotClient->write(px, w * h * 2);
    shotRow = area->y2 + 1;
  }
  lv_display_flush_ready(disp);
}

static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// Writes the screen to c as a top-down 16-bit BMP (RGB565, the draw buffer's own format).
size_t display_screenshot(WiFiClient& c) {
  lv_display_t* disp = lv_display_get_default();
  int w = lv_display_get_horizontal_resolution(disp), h = lv_display_get_vertical_resolution(disp);
  uint32_t data = (uint32_t)w * h * 2, size = 66 + data;
  uint8_t hd[66] = {'B', 'M'};
  put32(hd + 2, size);
  put32(hd + 10, 66);              // pixel data offset
  put32(hd + 14, 40);              // BITMAPINFOHEADER
  put32(hd + 18, w);
  put32(hd + 22, (uint32_t)-h);    // negative height: rows top to bottom
  put16(hd + 26, 1);
  put16(hd + 28, 16);
  put32(hd + 30, 3);               // BI_BITFIELDS
  put32(hd + 34, data);
  put32(hd + 54, 0xF800);          // red, green, blue masks
  put32(hd + 58, 0x07E0);
  put32(hd + 62, 0x001F);
  c.write(hd, sizeof(hd));
  shotClient = &c;
  shotRow = 0;
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(disp);
  shotClient = nullptr;
  // Rows LVGL didn't send in order (rare) are sent black, so the file is always complete.
  static const uint8_t zero[64] = {};
  for (uint32_t left = (uint32_t)(h - shotRow) * w * 2; left;) {
    size_t n = left < sizeof(zero) ? left : sizeof(zero);
    c.write(zero, n);
    left -= n;
  }
  LOG("screenshot: %dx%d, %d rows captured", w, h, shotRow);
  return size;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  LOG("boot, reset reason %d, loop stack %u", (int)esp_reset_reason(), (unsigned)getArduinoLoopTaskStackSize());
  settings_load();
  model_init();

  LOG("F.R.I.D.A.Y. %s", FRIDAY_VERSION);
  lv_init();
  lv_tick_set_cb(tick);
  lv_log_register_print_cb(lvLog);
  void* drawBuf = heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  lv_display_t* disp = lv_tft_espi_create(TFT_HOR_RES, TFT_VER_RES, drawBuf, DRAW_BUF_BYTES);
  lv_display_set_rotation(disp, SCREEN_ROTATION);
  lv_display_set_flush_cb(disp, flushWithCapture);
  ui_init();
  ui_apply_settings();

  ui_boot("Connecting to WiFi", "");
  WiFi.setHostname(HOSTNAME);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    // Reason codes: 2 AUTH_EXPIRE, 15 4WAY_HANDSHAKE_TIMEOUT, 23 802_1X_AUTH_FAILED, 201 NO_AP_FOUND, 202 AUTH_FAIL
    LOG("wifi disconnected, reason %d", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFiManager wm;
  // Order: 1) saved normal WiFi  2) company WiFi (PEAP)  3) own hotspot with the setup portal.
  bool online = tryNormal(wm) || tryEnterprise();

  if (!online) {
    // With a saved network the hotspot stays up 3 min, then the board restarts and tries it again,
    // so a router that was down comes back without anyone touching the board.
    bool hasSaved = wm.getWiFiIsSaved();
    wm.setConfigPortalTimeout(hasSaved ? 180 : 300);
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
    String hint = "Join the WiFi \"" WIFI_SETUP_AP "\" on your phone\nand open http://192.168.4.1";
    if (hasSaved) hint = "\"" + wm.getWiFiSSID(true) + "\" not reachable, retrying in 3 min.\n" + hint + " to change it";
    ui_boot("WiFi setup", hint);
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
  configTzTime(TZ_INFO, NTP_1, NTP_2, NTP_3);
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
  RefreshKind due = fetch_due();
  if (due != REFRESH_NONE) ui_refresh(due);
  // Daily restart for a clean heap (skipped in the first hour after boot, so it runs once).
  if (NIGHTLY_RESTART_HOUR >= 0 && millis() > 3600000UL) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    if (now > 1700000000 && lt.tm_hour == NIGHTLY_RESTART_HOUR && lt.tm_min == 0) {
      LOG("restart: nightly");
      Serial.flush();
      ESP.restart();
    }
  }
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    unsigned used, peak, overflow;
    lvgl_pool_stats(used, peak, overflow);
    LOG("alive, wifi %d, lvgl pool %uk used (peak %uk of %uk), overflow %u", (int)WiFi.status(), used, peak,
        (unsigned)(LVGL_POOL_BYTES / 1024), overflow);
  }
  delay(5);
}
