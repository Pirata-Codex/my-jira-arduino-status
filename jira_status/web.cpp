#include "web.h"
#include "config.h"
#include "settings.h"
#include "fetch.h"
#include "model.h"
#include "ui.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>

static WebServer server(80);
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
  h += F("<!doctype html><meta name=viewport content='width=device-width'><title>Jira Status</title><style>"
         "body{font:15px system-ui;background:#0f1117;color:#e6e8ef;max-width:640px;margin:auto;padding:16px}"
         "h1{font-size:20px}h2{font-size:15px;color:#6c8cff;margin-top:24px}label{display:block;margin:10px 0}"
         "input{display:block;width:100%;box-sizing:border-box;padding:8px;margin-top:4px;background:#1a1d27;"
         "color:#e6e8ef;border:1px solid #2a2f3d;border-radius:8px}small{color:#8a90a2}"
         "button{background:#6c8cff;color:#fff;border:0;padding:10px 18px;border-radius:8px;margin-top:16px}"
         ".err{color:#ff4d5e}a{color:#6c8cff}</style><h1>Jira Status board</h1>");
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
  h += F("<h2>Display</h2>");
  h += field("bright", "Brightness (0-255)", String(gCfg.brightness), "", "number");
  h += field("rotate", "Seconds per screen", String(gCfg.rotateSec), "", "number");
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
  gCfg.brightness = constrain(arg("bright").toInt(), 10, 255);
  gCfg.rotateSec = constrain(arg("rotate").toInt(), 4, 120);
  if (arg("webPass").length()) gCfg.webPass = arg("webPass");
  settings_save();
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
