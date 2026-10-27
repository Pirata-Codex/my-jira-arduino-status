#include "web.h"
#include "config.h"
#include "settings.h"
#include "fetch.h"
#include "model.h"
#include "ui.h"
#include "memory.h"
#include "liveview.h"
#include <lvgl.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>

static WebServer server(80);
size_t display_screenshot(WiFiClient& c);  // jira_status.ino
static HTTPUpdateServer updater;

// Live view page: the screen, drawn from the board's stream, and the room sound level beside it.
static const char STREAM_PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>F.R.I.D.A.Y. live</title>
<style>body{margin:0;background:#060D16;color:#ECEEF4;font:14px ui-monospace,monospace;padding:16px}
#w{display:flex;flex-wrap:wrap;gap:16px;align-items:flex-start}
canvas#s{width:100%;max-width:960px;image-rendering:pixelated;border:1px solid #1C6A80;border-radius:8px}
#m{border:1px solid #1C6A80;border-radius:8px;padding:12px;min-width:220px}
#db{font-size:44px}#st{font-size:16px;letter-spacing:2px}#g{width:220px;height:80px;display:block;margin-top:10px}
#i{color:#86AABA;margin-top:10px}a{color:#22D3EE}</style>
<div id=w><canvas id=s width=480 height=320></canvas>
<div id=m>ROOM SOUND<div id=db>--</div><div id=st></div><canvas id=g width=220 height=80></canvas>
<div style="color:#86AABA">last 2 minutes</div></div></div>
<div id=i>connecting...</div><p><a href=/>Settings</a> · <a href=/screenshot>Screenshot</a></p>
<script>
const cv=document.getElementById('s'),cx=cv.getContext('2d'),info=document.getElementById('i');
const g=document.getElementById('g'),gx=g.getContext('2d'),dbE=document.getElementById('db'),stE=document.getElementById('st');
const NAMES=['FOCUS','NORMAL','DISTRACTING','TOO LOUD'],COLS=['#A3E635','#22D3EE','#F5A623','#FF4D5E'];
let hist=[],bytes=0,frames=0;
function sound(db,st){
  if(st<0){dbE.textContent='--';stE.textContent='NO MICROPHONE';return}
  dbE.textContent=db.toFixed(0)+' dB';dbE.style.color=stE.style.color=COLS[st];stE.textContent=NAMES[st];
  hist.push([db,st]);if(hist.length>120)hist.shift();
  gx.clearRect(0,0,220,80);
  hist.forEach((h,k)=>{const y=Math.max(2,Math.min(78,80-(h[0]-30)*80/50));gx.fillStyle=COLS[h[1]];gx.fillRect(k*220/120,y,2,80-y)});
}
async function run(){
  const r=await fetch('/stream.raw',{cache:'no-store'});
  if(!r.ok){info.textContent=await r.text();return false}
  const rd=r.body.getReader();let chunks=[],have=0;
  async function take(n){
    while(have<n){const {value,done}=await rd.read();if(done)throw new Error('closed');chunks.push(value);have+=value.length;bytes+=value.length}
    const out=new Uint8Array(n);let o=0;
    while(o<n){const c=chunks[0],k=Math.min(c.length,n-o);out.set(c.subarray(0,k),o);o+=k;
      if(k===c.length)chunks.shift();else chunks[0]=c.subarray(k)}
    have-=n;return out}
  const hd=await take(8);const v=new DataView(hd.buffer);
  cv.width=v.getUint16(4,true);cv.height=v.getUint16(6,true);
  info.textContent='live';
  for(;;){
    const t=(await take(1))[0];
    if(t===1){
      const h=new DataView((await take(8)).buffer),x=h.getUint16(0,true),y=h.getUint16(2,true),w=h.getUint16(4,true),hh=h.getUint16(6,true);
      const px=await take(w*hh*2),img=cx.createImageData(w,hh),d=img.data;
      for(let i=0,j=0;i<px.length;i+=2,j+=4){const c=px[i]|px[i+1]<<8;
        d[j]=(c>>8&0xF8)|c>>13;d[j+1]=(c>>3&0xFC)|(c>>9&3);d[j+2]=(c<<3&0xF8)|(c>>2&7);d[j+3]=255}
      cx.putImageData(img,x,y);frames++;
    }else if(t===2){
      const p=new DataView((await take(3)).buffer);sound(p.getUint16(0,true)/10,p.getInt8(2));
    }else throw new Error('bad packet');
  }
}
setInterval(()=>{if(info.textContent.startsWith('live'))info.textContent='live · '+(bytes/1024).toFixed(0)+' KB/s · '+frames+' updates/s';bytes=frames=0},1000);
(async()=>{for(;;){try{if(await run()===false)return}catch(e){info.textContent='reconnecting...'}await new Promise(r=>setTimeout(r,2000))}})();
</script>)HTML";

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
  h += field("jiraUrl2", "Jira URL (backup)", gCfg.jiraUrl2, "Optional. Tried when the first one fails, e.g. a relay at home: http://192.168.137.1:8080");
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
  h += field("kumaUrl2", "Kuma URL (backup)", gCfg.kumaUrl2, "Optional, e.g. http://192.168.137.1:8081");
  h += field("kumaSlug", "Status page slug", gCfg.kumaSlug);
  h += F("<h2>Meetings</h2>");
  h += field("icsUrl", "Calendar .ics URL", gCfg.icsUrl, "Outlook: Settings → Calendar → Shared calendars → Publish → ICS link");
  h += field("icsUrl2", "Calendar .ics URL (backup)", gCfg.icsUrl2, "Optional: the same link through another host, e.g. http://192.168.137.1:8082/owa/calendar/...");
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
  h += field("micCal", "Microphone calibration (dB)", String(gCfg.micCal),
             "Added to the measured level. Set it so a quiet room shows about 35-40 dB in the header", "number");
  h += field("rfidUids", "Badge ids", gCfg.rfidUids, "Your RFID badges (hex, comma separated). Tap an unknown badge: the board shows its id");
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
  gCfg.jiraUrl2 = arg("jiraUrl2");
  gCfg.kumaUrl2 = arg("kumaUrl2");
  gCfg.icsUrl2 = arg("icsUrl2");
  gCfg.rfidUids = arg("rfidUids");
  gCfg.micCal = constrain(arg("micCal").toInt(), -40, 90);
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
  // Live view: /stream is the page, /stream.raw the data it draws (see liveview.cpp).
  server.on("/stream", HTTP_GET, [] {
    if (!auth()) return;
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", STREAM_PAGE);
  });
  server.on("/stream.raw", HTTP_GET, [] {
    if (!auth()) return;
    WiFiClient c = server.client();
    if (!stream_start(c))
      server.send(503, "text/plain", "Live view needs PSRAM (ESP32-S3 with PSRAM enabled in Tools > PSRAM).");
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
