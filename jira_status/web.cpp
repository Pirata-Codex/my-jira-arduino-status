#include "web.h"
#include "config.h"
#include "settings.h"
#include "fetch.h"
#include "model.h"
#include "ui.h"
#include "memory.h"
#include <lvgl.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>

static WebServer server(80);
size_t display_screenshot(WiFiClient& c);  // jira_status.ino
static HTTPUpdateServer updater;

static String esc(const String& s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '&') o += "&amp;";
    else o += c;
  }
  return o;
}

static String field(const char* name, const char* label, const String& value, const char* hint = "", const char* type = "text") {
  return String("<label>") + label + "<input type='" + type + "' name='" + name + "' value='" + esc(value) + "'>" +
         (*hint ? String("<small>") + hint + "</small>" : String("")) + "</label>";
}

static bool auth() {
  if (server.authenticate("admin", gCfg.webPass.c_str())) return true;
  server.requestAuthentication();
  return false;
}

static void handleRoot() {
  if (!auth()) return;
  model_lock();
  String sprintErr = gData.sprint.err, healthErr = gData.health.err, meetErr = gData.meet.err;
  bool mock = gData.mock;
  model_unlock();

  String h;
  h.reserve(6000);
  h += F("<!doctype html><meta name=viewport content='width=device-width'><title>F.R.I.D.A.Y.</title><style>"
         "body{font:15px system-ui;background:#0f1117;color:#e6e8ef;max-width:640px;margin:auto;padding:16px}"
         "h1{font-size:20px}h2{font-size:15px;color:#6c8cff;margin-top:24px}label{display:block;margin:10px 0}"
         "input{display:block;width:100%;box-sizing:border-box;padding:8px;margin-top:4px;background:#1a1d27;"
         "color:#e6e8ef;border:1px solid #2a2f3d;border-radius:8px}small{color:#8a90a2}"
         "button{background:#6c8cff;color:#fff;border:0;padding:10px 18px;border-radius:8px;margin-top:16px}"
         ".err{color:#ff4d5e}a{color:#6c8cff}</style><h1>F.R.I.D.A.Y. · Jira status board</h1>");
  h += "<p><small>Free memory " + String(ESP.getFreeHeap() / 1024) + " KB, largest block " +
       String(ESP.getMaxAllocHeap() / 1024) + " KB (HTTPS needs about 40 KB)</small></p>";
  if (mock) h += F("<p class=err>Demo data: set the Jira URL and token to go live.</p>");
  if (sprintErr.length()) h += "<p class=err>Jira: " + esc(sprintErr) + "</p>";
  if (healthErr.length()) h += "<p class=err>Kuma: " + esc(healthErr) + "</p>";
  if (meetErr.length()) h += "<p class=err>Calendar: " + esc(meetErr) + "</p>";
  h += F("<form method=post action=/save><h2>Jira</h2>");
  h += field("jiraUrl", "Jira URL", gCfg.jiraUrl, "e.g. https://jira.example.com");
  // The token is never sent back to the browser; leave empty to keep the stored one.
  h += field("jiraPat", "Personal access token", "", gCfg.jiraPat.length() ? "A token is stored. Leave empty to keep it." : "Jira profile → Personal Access Tokens", "password");
  h += field("jql", "My sprint cards (JQL)", gCfg.jql);
  h += field("spField", "Story points field", gCfg.spField, "e.g. customfield_10006. Empty = every card counts 1");
  h += field("boardId", "Board id", gCfg.boardId, "From the board URL (rapidView=…). Gives the sprint name and end date");
  h += F("<h2>KPI tiles (JQL counts)</h2>");
  h += field("k1Label", "KPI 1 label", gCfg.kpi1Label);
  h += field("k1Jql", "KPI 1 JQL", gCfg.kpi1Jql);
  h += field("k2Label", "KPI 2 label", gCfg.kpi2Label);
  h += field("k2Jql", "KPI 2 JQL", gCfg.kpi2Jql);
  h += F("<h2>Health (Uptime Kuma)</h2>");
  h += field("kumaUrl", "Kuma URL", gCfg.kumaUrl);
  h += field("kumaSlug", "Status page slug", gCfg.kumaSlug);
  h += F("<h2>Meetings</h2>");
  h += field("icsUrl", "Calendar .ics URL", gCfg.icsUrl, "Outlook: Settings → Calendar → Shared calendars → Publish → ICS link");
  h += F("<h2>Company WiFi (PEAP)</h2><p><small>Tried first on every boot; the normal WiFi is the fallback. "
         "Saving restarts the board.</small></p>");
  h += field("entSsid", "Network name (SSID)", gCfg.entSsid, "Empty = don't use company WiFi");
  h += field("entUser", "Username", gCfg.entUser, "As on your laptop, e.g. r.khaleghi or DOMAIN\\r.khaleghi");
  h += field("entId", "Outer identity", gCfg.entIdentity, "Usually empty (then the username is used)");
  h += field("entPass", "Password", "", gCfg.entPass.length() ? "A password is stored. Leave empty to keep it." : "", "password");
  h += "<label>CA certificate (PEM, optional)<textarea name=entCa rows=5 style='width:100%;background:#1a1d27;color:#e6e8ef;"
       "border:1px solid #2a2f3d;border-radius:8px'>" + esc(gCfg.entCa) + "</textarea>"
       "<small>Paste -----BEGIN CERTIFICATE----- ... to verify the server. Empty = connect without checking it.</small></label>";
  h += F("<h2>Display</h2>");
  h += field("bright", "Brightness (0-255)", String(gCfg.brightness), "", "number");
  h += field("rotate", "Seconds per screen", String(gCfg.rotateSec), "", "number");
  h += field("refresh", "Refresh Jira + calendar every (minutes)", String(gCfg.refreshMin), "The dashboard pauses for a few seconds while it refreshes", "number");
  h += field("kumaMin", "Refresh Kuma health every (minutes)", String(gCfg.kumaMin), "", "number");
  h += field("owner", "Your name", gCfg.ownerName, "F.R.I.D.A.Y. greets you with it");
  h += F("<h2>Cards slide</h2>");
  h += field("cardsPP", "Cards per page (1-5)", String(gCfg.cardsPerPage), "", "number");
  h += field("cardSec", "Seconds per page", String(gCfg.cardPageSec), "", "number");
  h += String("<label><input type=checkbox name=hideDone value=1 style='display:inline;width:auto'") +
       (gCfg.hideDone ? " checked" : "") + "> Hide Done cards</label>";
  h += field("webPass", "Page password (user admin)", "", "Leave empty to keep the current one", "password");
  h += F("<button>Save</button></form><p><a href=/update>Firmware update</a></p>");
  server.send(200, "text/html", h);
}

static void handleSave() {
  if (!auth()) return;
  auto arg = [](const char* n) { String v = server.arg(n); v.trim(); return v; };
  gCfg.jiraUrl = arg("jiraUrl");
  if (arg("jiraPat").length()) gCfg.jiraPat = arg("jiraPat");
  gCfg.jql = arg("jql");
  gCfg.spField = arg("spField");
  gCfg.boardId = arg("boardId");
  gCfg.kpi1Label = arg("k1Label");
  gCfg.kpi1Jql = arg("k1Jql");
  gCfg.kpi2Label = arg("k2Label");
  gCfg.kpi2Jql = arg("k2Jql");
  gCfg.kumaUrl = arg("kumaUrl");
  gCfg.kumaSlug = arg("kumaSlug");
  gCfg.icsUrl = arg("icsUrl");
  gCfg.icsUrl.trim();
  gCfg.brightness = constrain(arg("bright").toInt(), 10, 255);
  gCfg.rotateSec = constrain(arg("rotate").toInt(), 4, 120);
  gCfg.refreshMin = constrain(arg("refresh").toInt(), 2, 240);
  gCfg.kumaMin = constrain(arg("kumaMin").toInt(), 1, 240);
  gCfg.cardsPerPage = constrain(arg("cardsPP").toInt(), 1, 5);
  gCfg.cardPageSec = constrain(arg("cardSec").toInt(), 2, 60);
  gCfg.hideDone = server.hasArg("hideDone");
  gCfg.ownerName = arg("owner");
  if (gCfg.ownerName.isEmpty()) gCfg.ownerName = "Boss";
  if (arg("webPass").length()) gCfg.webPass = arg("webPass");
  String oldEnt = gCfg.entSsid + "|" + gCfg.entUser + "|" + gCfg.entIdentity + "|" + gCfg.entPass + "|" + gCfg.entCa;
  gCfg.entSsid = arg("entSsid");
  gCfg.entUser = arg("entUser");
  gCfg.entIdentity = arg("entId");
  if (arg("entPass").length()) gCfg.entPass = arg("entPass");
  gCfg.entCa = server.arg("entCa");
  gCfg.entCa.trim();
  bool wifiChanged = oldEnt != gCfg.entSsid + "|" + gCfg.entUser + "|" + gCfg.entIdentity + "|" + gCfg.entPass + "|" + gCfg.entCa;
  settings_save();
  if (wifiChanged) {
    server.send(200, "text/html", "<meta http-equiv=refresh content='20;url=/'>Saved. Restarting to join the company WiFi...");
    delay(500);
    ESP.restart();
  }
  ui_apply_settings();
  fetch_request_refresh();
  server.sendHeader("Location", "/");
  server.send(303);
}

void web_begin() {
  MDNS.begin(HOSTNAME);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/refresh", HTTP_GET, [] {
    if (!auth()) return;
    fetch_request_refresh();
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/memory", HTTP_GET, [] {
    if (!auth()) return;
    if (server.hasArg("reset")) {
      mem_reset_learned();
      server.sendHeader("Location", "/memory");
      server.send(303);
      return;
    }
    server.send(200, "text/html", "<meta name=viewport content='width=device-width'><style>body{font-family:sans-serif;margin:16px}"
                "td,th{padding:3px 8px;border-bottom:1px solid #ddd;text-align:left}</style>" + mem_report_html() +
                "<p><a href='/'>Settings</a></p>");
  });
  server.on("/screenshot", HTTP_GET, [] {
    if (!auth()) return;
    lv_display_t* disp = lv_display_get_default();
    size_t size = 66 + (size_t)lv_display_get_horizontal_resolution(disp) * lv_display_get_vertical_resolution(disp) * 2;
    server.setContentLength(size);
    server.sendHeader("Content-Disposition", "inline; filename=board.bmp");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "image/bmp", "");
    WiFiClient c = server.client();
    display_screenshot(c);
  });
  updater.setup(&server, "/update", "admin", gCfg.webPass.c_str());
  server.begin();
  MDNS.addService("http", "tcp", 80);

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(gCfg.webPass.c_str());
  ArduinoOTA.begin();
}

void web_loop() {
  server.handleClient();
  ArduinoOTA.handle();
}
