#include "fetch.h"
#include "config.h"
#include "model.h"
#include "settings.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <algorithm>

static volatile bool refreshNow = true;
void fetch_request_refresh() { refreshNow = true; }

// ---------- helpers ----------

static String urlEncode(const String& s) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

static String trimSlash(String s) {
  while (s.endsWith("/")) s.remove(s.length() - 1);
  return s;
}

// Days since 1970-01-01 for a civil date (no timegm() on the ESP32).
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

static time_t utcEpoch(int y, int mo, int d, int h, int mi, int s) {
  return (time_t)daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

static time_t localEpoch(int y, int mo, int d, int h, int mi, int s) {
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = mi; t.tm_sec = s; t.tm_isdst = -1;
  return mktime(&t);
}

// Jira dates: 2026-10-30T12:00:00.000+03:30
static time_t parseIso(const char* s) {
  if (!s || strlen(s) < 19) return 0;
  int y, mo, d, h, mi, sec;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
  time_t t = utcEpoch(y, mo, d, h, mi, sec);
  const char* tz = s + 19;
  while (*tz == '.' || isdigit((uint8_t)*tz)) tz++;
  if (*tz == '+' || *tz == '-') {
    // +0330 or +03:30
    char digits[5] = {};
    int n = 0;
    for (const char* p = tz + 1; *p && n < 4; p++) if (isdigit((uint8_t)*p)) digits[n++] = *p;
    int hhmm = atoi(digits);
    int off = (hhmm / 100) * 3600 + (hhmm % 100) * 60;
    t += (*tz == '+') ? -off : off;
  }
  return t;
}

// iCalendar dates: 20261023T100000Z (UTC), 20261023T100000 (local), 20261023 (all-day, skipped)
static time_t parseIcsDate(const String& v, bool& allDay) {
  allDay = false;
  int y, mo, d, h = 0, mi = 0, s = 0;
  if (v.length() < 8 || sscanf(v.c_str(), "%4d%2d%2d", &y, &mo, &d) != 3) return 0;
  int t = v.indexOf('T');
  if (t < 0) { allDay = true; return localEpoch(y, mo, d, 0, 0, 0); }
  sscanf(v.c_str() + t + 1, "%2d%2d%2d", &h, &mi, &s);
  return v.endsWith("Z") ? utcEpoch(y, mo, d, h, mi, s) : localEpoch(y, mo, d, h, mi, s);
}

static bool begin(HTTPClient& http, WiFiClientSecure& tls, WiFiClient& plain, const String& url) {
  // Internal services often use a private CA, so the certificate isn't pinned.
  tls.setInsecure();
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);  // no chunked encoding, so the body can be streamed into the parser
  return url.startsWith("https") ? http.begin(tls, url) : http.begin(plain, url);
}

static bool getJson(const String& url, const String& bearer, JsonDocument& doc, const JsonDocument* filter, String& err) {
  HTTPClient http;
  WiFiClientSecure tls;
  WiFiClient plain;
  if (!begin(http, tls, plain, url)) { err = "bad url"; return false; }
  if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  if (code != 200) {
    err = code > 0 ? "HTTP " + String(code) : String(http.errorToString(code));
    if (code < 0) {
      // Say why: a TLS error (from mbedTLS) or too little memory for the handshake.
      char tlsErr[100] = "";
      if (url.startsWith("https")) tls.lastError(tlsErr, sizeof(tlsErr));
      if (*tlsErr) err += String(" / tls: ") + tlsErr;
      err += " / heap " + String(ESP.getFreeHeap() / 1024) + "k, block " + String(ESP.getMaxAllocHeap() / 1024) + "k";
    }
    http.end();
    return false;
  }
  DeserializationError e = filter
      ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter))
      : deserializeJson(doc, http.getStream());
  http.end();
  if (e) { err = String("json: ") + e.c_str(); return false; }
  return true;
}

// ---------- Jira ----------

static CardCat categorize(const String& statusName, const char* catKey, bool& blocked) {
  String n = statusName;
  n.toLowerCase();
  blocked = n.indexOf("block") >= 0 || n.indexOf("hold") >= 0;
  if (catKey && strcmp(catKey, "done") == 0) return CAT_DONE;
  if (n.indexOf("review") >= 0 || n.indexOf("test") >= 0 || n.indexOf("qa") >= 0) return CAT_REVIEW;
  if (catKey && strcmp(catKey, "indeterminate") == 0 && !blocked) return CAT_PROGRESS;
  return CAT_TODO;
}

static int jiraCount(const String& jql, String& err) {
  if (jql.isEmpty()) return -1;
  JsonDocument doc;
  String url = trimSlash(gCfg.jiraUrl) + "/rest/api/2/search?maxResults=0&jql=" + urlEncode(jql);
  if (!getJson(url, gCfg.jiraPat, doc, nullptr, err)) return -1;
  return doc["total"] | -1;
}

// Keeps the last BURN_DAYS "points remaining" values in NVS, one per day, reset when the sprint changes.
static void updateBurndown(SprintData& s) {
  struct tm lt;
  time_t now = time(nullptr);
  localtime_r(&now, &lt);
  uint32_t today = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
  Preferences p;
  p.begin("burn", false);
  String sprint = p.getString("sprint", "");
  uint32_t last = p.getUInt("day", 0);
  float vals[BURN_DAYS] = {};
  int len = 0;
  if (sprint == s.name) {
    len = min((int)p.getUChar("len", 0), BURN_DAYS);
    p.getBytes("vals", vals, sizeof(vals));
  }
  float remaining = s.ptsTotal - s.ptsDone;
  if (len > 0 && last == today) {
    vals[len - 1] = remaining;
  } else {
    if (len == BURN_DAYS) { memmove(vals, vals + 1, sizeof(float) * (BURN_DAYS - 1)); len--; }
    vals[len++] = remaining;
  }
  p.putString("sprint", s.name);
  p.putUInt("day", today);
  p.putUChar("len", len);
  p.putBytes("vals", vals, sizeof(vals));
  p.end();
  memcpy(s.burn, vals, sizeof(vals));
  s.burnLen = len;
}

static void fetchJira(SprintData& s) {
  s = SprintData();
  String base = trimSlash(gCfg.jiraUrl);
  JsonDocument filter;
  JsonObject f = filter["issues"][0].to<JsonObject>();
  f["key"] = true;
  f["fields"]["summary"] = true;
  f["fields"]["status"]["name"] = true;
  f["fields"]["status"]["statusCategory"]["key"] = true;
  if (gCfg.spField.length()) f["fields"][gCfg.spField] = true;
  filter["total"] = true;

  String fields = "summary,status";
  if (gCfg.spField.length()) fields += "," + gCfg.spField;
  String url = base + "/rest/api/2/search?maxResults=" + String(MAX_CARDS) + "&fields=" + fields + "&jql=" + urlEncode(gCfg.jql);
  JsonDocument doc;
  if (!getJson(url, gCfg.jiraPat, doc, &filter, s.err)) return;

  for (JsonObject issue : doc["issues"].as<JsonArray>()) {
    if (s.count >= MAX_CARDS) break;
    Card& c = s.cards[s.count++];
    c.key = issue["key"].as<const char*>();
    c.title = issue["fields"]["summary"].as<const char*>();
    c.status = issue["fields"]["status"]["name"].as<const char*>();
    c.cat = categorize(c.status, issue["fields"]["status"]["statusCategory"]["key"], c.blocked);
    // Without a story-points field every card counts as one point.
    c.pts = gCfg.spField.length() ? (issue["fields"][gCfg.spField] | 0.0f) : 1.0f;
    s.ptsTotal += c.pts;
    if (c.cat == CAT_DONE) s.ptsDone += c.pts;
  }

  if (gCfg.boardId.length()) {
    JsonDocument sp;
    String err;
    if (getJson(base + "/rest/agile/1.0/board/" + gCfg.boardId + "/sprint?state=active", gCfg.jiraPat, sp, nullptr, err)) {
      JsonObject v = sp["values"][0];
      s.name = v["name"] | "";
      s.endTs = parseIso(v["endDate"]);
    }
  }
  if (s.name.isEmpty()) s.name = "Current sprint";

  String err;
  s.kpi1 = jiraCount(gCfg.kpi1Jql, err);
  s.kpi2 = jiraCount(gCfg.kpi2Jql, err);
  s.ok = true;
  s.updated = time(nullptr);
  updateBurndown(s);
}

// ---------- Uptime Kuma (public status page API, no auth) ----------

static void fetchKuma(HealthData& h) {
  h = HealthData();
  if (gCfg.kumaUrl.isEmpty() || gCfg.kumaSlug.isEmpty()) { h.err = "not configured"; return; }
  String base = trimSlash(gCfg.kumaUrl);

  JsonDocument f1;
  JsonObject mf = f1["publicGroupList"][0]["monitorList"][0].to<JsonObject>();
  mf["id"] = true;
  mf["name"] = true;
  JsonDocument page;
  if (!getJson(base + "/api/status-page/" + gCfg.kumaSlug, "", page, &f1, h.err)) return;

  // Heartbeats are oldest-first; keep only status and ping to save RAM.
  JsonDocument f2;
  JsonObject beat = f2["heartbeatList"]["*"][0].to<JsonObject>();
  beat["status"] = true;
  beat["ping"] = true;
  f2["uptimeList"]["*"] = true;
  JsonDocument beats;
  if (!getJson(base + "/api/status-page/heartbeat/" + gCfg.kumaSlug, "", beats, &f2, h.err)) return;

  for (JsonObject group : page["publicGroupList"].as<JsonArray>()) {
    for (JsonObject mon : group["monitorList"].as<JsonArray>()) {
      if (h.count >= MAX_MONITORS) break;
      Monitor& m = h.m[h.count++];
      String id = String(mon["id"].as<int>());
      m.name = mon["name"].as<const char*>();
      JsonArray list = beats["heartbeatList"][id];
      if (list.size()) {
        JsonObject last = list[list.size() - 1];
        int status = last["status"] | 1;   // 0 down, 1 up, 2 pending, 3 maintenance
        m.up = status != 0;
        m.ping = last["ping"] | -1;
      }
      m.uptime = beats["uptimeList"][id + "_24"] | -1.0f;
      if (m.up) h.up++;
    }
  }
  h.ok = true;
  h.updated = time(nullptr);
}

// ---------- Calendar (.ics, streamed line by line) ----------

static void addMeeting(MeetingData& d, const Meeting& m) {
  if (d.count < MAX_MEETINGS) { d.m[d.count++] = m; return; }
  // Full: replace the latest one if this starts earlier.
  int latest = 0;
  for (int i = 1; i < d.count; i++) if (d.m[i].start > d.m[latest].start) latest = i;
  if (m.start < d.m[latest].start) d.m[latest] = m;
}

static void handleIcsLine(const String& line, MeetingData& d, Meeting& cur, bool& inEvent, bool& allDay, time_t dayStart, time_t dayEnd) {
  if (line == "BEGIN:VEVENT") { cur = Meeting(); inEvent = true; allDay = false; return; }
  if (!inEvent) return;
  if (line == "END:VEVENT") {
    inEvent = false;
    if (!allDay && cur.start >= dayStart && cur.start < dayEnd) {
      if (!cur.end) cur.end = cur.start + 30 * 60;
      addMeeting(d, cur);
    }
    return;
  }
  int colon = line.indexOf(':');
  if (colon < 0) return;
  String name = line.substring(0, colon);
  String value = line.substring(colon + 1);
  int semi = name.indexOf(';');
  if (semi >= 0) name = name.substring(0, semi);
  bool ad;
  if (name == "DTSTART") { cur.start = parseIcsDate(value, ad); allDay = ad; }
  else if (name == "DTEND") cur.end = parseIcsDate(value, ad);
  else if (name == "SUMMARY") { value.replace("\\,", ","); cur.title = value.substring(0, 80); }
  else if (name == "LOCATION") { value.replace("\\,", ","); cur.where = value.substring(0, 60); }
}

static void fetchCalendar(MeetingData& d) {
  d = MeetingData();
  if (gCfg.icsUrl.isEmpty()) return;
  d.configured = true;

  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  time_t dayStart = localEpoch(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, 0, 0, 0);
  time_t dayEnd = dayStart + 86400;

  HTTPClient http;
  WiFiClientSecure tls;
  WiFiClient plain;
  if (!begin(http, tls, plain, gCfg.icsUrl)) { d.err = "bad url"; return; }
  int code = http.GET();
  if (code != 200) { d.err = "HTTP " + String(code); http.end(); return; }

  WiFiClient& stream = http.getStream();
  Meeting cur;
  bool inEvent = false, allDay = false;
  String pending;
  uint32_t deadline = millis() + 20000;
  while ((stream.connected() || stream.available()) && millis() < deadline) {
    if (!stream.available()) { delay(5); continue; }
    String line = stream.readStringUntil('\n');
    // Folded lines continue the previous one with a leading space.
    bool folded = line.length() && (line[0] == ' ' || line[0] == '\t');
    line.trim();
    if (folded && pending.length()) { if (pending.length() < 200) pending += line; continue; }
    if (pending.length()) handleIcsLine(pending, d, cur, inEvent, allDay, dayStart, dayEnd);
    pending = line.substring(0, 200);
  }
  if (pending.length()) handleIcsLine(pending, d, cur, inEvent, allDay, dayStart, dayEnd);
  http.end();

  std::sort(d.m, d.m + d.count, [](const Meeting& a, const Meeting& b) { return a.start < b.start; });
  d.ok = true;
  d.updated = now;
}

// ---------- task ----------

static bool timeValid() { return time(nullptr) > 1700000000; }

template <typename T, typename Fn>
static void publish(T Snapshot::*field, Fn fill) {
  T* fresh = new T();
  fill(*fresh);
  model_lock();
  // Keep the last good data on a failed refresh; only record the error.
  if (fresh->ok || !(gData.*field).ok) gData.*field = *fresh;
  else (gData.*field).err = fresh->err;
  gData.version++;
  model_unlock();
  delete fresh;
}

static void task(void*) {
  uint32_t lastJira = 0, lastKuma = 0, lastCal = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && timeValid()) {
      bool all = refreshNow;
      refreshNow = false;
      uint32_t now = millis();
      bool mock = settings_mock();
      if (mock && (all || !gData.mock)) {
        Snapshot* m = new Snapshot();
        model_fill_mock(*m);
        model_lock();
        gData.sprint = m->sprint;
        gData.meet = m->meet;
        gData.mock = true;
        gData.version++;
        model_unlock();
        delete m;
      }
      if (!mock && gData.mock) {
        // Leaving demo mode: drop the demo data so a failing first fetch shows its error, not demo cards.
        model_lock();
        gData.sprint = SprintData();
        gData.meet = MeetingData();
        gData.sprint.err = "Loading from Jira...";
        gData.mock = false;
        gData.version++;
        model_unlock();
      }
      if (!mock && (all || now - lastJira > FETCH_JIRA_MS)) {
        publish(&Snapshot::sprint, fetchJira);
        lastJira = now;
      }
      if (all || now - lastKuma > FETCH_KUMA_MS) { publish(&Snapshot::health, fetchKuma); lastKuma = now; }
      if (!mock && (all || now - lastCal > FETCH_CALENDAR_MS)) { publish(&Snapshot::meet, fetchCalendar); lastCal = now; }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void fetch_begin() {
  // TLS needs a deep stack. Core 0 keeps network work away from the UI loop on core 1.
  xTaskCreatePinnedToCore(task, "fetch", 16 * 1024, nullptr, 1, nullptr, 0);
}
