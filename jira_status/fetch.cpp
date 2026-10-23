#include "fetch.h"
#include "config.h"
#include "model.h"
#include "settings.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>

static volatile bool refreshNow = true;
static uint32_t lastFetch = 0, lastKuma = 0;
static FetchProgress progress = nullptr;
void fetch_request_refresh() { refreshNow = true; }

static void report(FetchStep step, StepState state, const String& detail = "") {
  if (progress) progress(step, state, detail);
}

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

static bool getJson(const String& url, const String& bearer, JsonDocument& doc, const JsonDocument* filter, String& err);

static int timedGet(HTTPClient& http) {
  uint32_t t0 = millis();
  int code = http.GET();
  LOG("  GET -> %d in %lu ms", code, millis() - t0);
  return code;
}

static bool getJsonUnlocked(const String& url, const String& bearer, JsonDocument& doc, const JsonDocument* filter, String& err) {
  HTTPClient http;
  WiFiClientSecure tls;
  WiFiClient plain;
  if (!begin(http, tls, plain, url)) { err = "bad url"; return false; }
  if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
  http.addHeader("Accept", "application/json");
  int code = timedGet(http);
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
  JsonDocument filter;
  filter["total"] = true;
  JsonDocument doc;
  String url = trimSlash(gCfg.jiraUrl) + "/rest/api/2/search?maxResults=0&fields=key&jql=" + urlEncode(jql);
  if (!getJson(url, gCfg.jiraPat, doc, &filter, err)) return -1;
  return doc["total"] | -1;
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
  f["fields"]["resolutiondate"] = true;
  if (gCfg.spField.length()) f["fields"][gCfg.spField] = true;
  filter["total"] = true;

  String fields = "summary,status,resolutiondate";
  if (gCfg.spField.length()) fields += "," + gCfg.spField;
  String url = base + "/rest/api/2/search?maxResults=" + String(MAX_CARDS) + "&fields=" + fields + "&jql=" + urlEncode(gCfg.jql);
  JsonDocument doc;
  report(STEP_CARDS, STEP_RUN);
  if (!getJson(url, gCfg.jiraPat, doc, &filter, s.err)) {
    report(STEP_CARDS, STEP_FAIL, s.err);
    report(STEP_SPRINT, STEP_SKIP);
    report(STEP_KPI, STEP_SKIP);
    return;
  }

  for (JsonObject issue : doc["issues"].as<JsonArray>()) {
    if (s.count >= MAX_CARDS) break;
    Card& c = s.cards[s.count++];
    c.key = issue["key"].as<const char*>();
    c.title = issue["fields"]["summary"].as<const char*>();
    c.status = issue["fields"]["status"]["name"].as<const char*>();
    c.cat = categorize(c.status, issue["fields"]["status"]["statusCategory"]["key"], c.blocked);
    c.resolved = c.cat == CAT_DONE ? parseIso(issue["fields"]["resolutiondate"]) : 0;
    // Without a story-points field every card counts as one point.
    c.pts = gCfg.spField.length() ? (issue["fields"][gCfg.spField] | 0.0f) : 1.0f;
    s.ptsTotal += c.pts;
    if (c.cat == CAT_DONE) s.ptsDone += c.pts;
  }

  report(STEP_CARDS, STEP_OK, String(s.count) + " cards, " + String(s.ptsDone, 1) + " / " + String(s.ptsTotal, 1) + " pts");

  // The extra calls below can fail on their own (each one is a new TLS handshake and RAM is tight).
  // Then keep the last good value instead of blanking the burndown or a KPI tile.
  model_lock();
  String prevName = gData.sprint.name;
  time_t prevStart = gData.sprint.startTs, prevEnd = gData.sprint.endTs;
  int prevKpi1 = gData.sprint.kpi1, prevKpi2 = gData.sprint.kpi2;
  bool prevReal = !gData.mock;
  model_unlock();

  if (!gCfg.boardId.length()) report(STEP_SPRINT, STEP_SKIP, "no board id");
  if (gCfg.boardId.length()) {
    report(STEP_SPRINT, STEP_RUN);
    JsonDocument spFilter;
    JsonObject sv = spFilter["values"][0].to<JsonObject>();
    sv["name"] = true;
    sv["startDate"] = true;
    sv["endDate"] = true;
    JsonDocument sp;
    String err;
    if (getJson(base + "/rest/agile/1.0/board/" + gCfg.boardId + "/sprint?state=active", gCfg.jiraPat, sp, &spFilter, err)) {
      JsonObject v = sp["values"][0];
      s.name = v["name"] | "";
      s.startTs = parseIso(v["startDate"]);
      s.endTs = parseIso(v["endDate"]);
      report(STEP_SPRINT, STEP_OK, s.name);
    } else {
      report(STEP_SPRINT, STEP_FAIL, err);
      LOG("jira sprint dates failed: %s (keeping last)", err.c_str());
      if (prevReal) { s.name = prevName; s.startTs = prevStart; s.endTs = prevEnd; }
    }
  }
  if (s.name.isEmpty()) s.name = "Current sprint";

  report(STEP_KPI, STEP_RUN);
  String err, kpiErr;
  s.kpi1 = jiraCount(gCfg.kpi1Jql, err);
  if (s.kpi1 < 0 && gCfg.kpi1Jql.length()) kpiErr = err;
  if (s.kpi1 < 0 && gCfg.kpi1Jql.length()) {
    LOG("jira kpi1 failed: %s (keeping last)", err.c_str());
    if (prevReal) s.kpi1 = prevKpi1;
  }
  err = "";
  s.kpi2 = jiraCount(gCfg.kpi2Jql, err);
  if (s.kpi2 < 0 && gCfg.kpi2Jql.length()) kpiErr = err;
  if (kpiErr.length()) report(STEP_KPI, STEP_FAIL, kpiErr);
  else report(STEP_KPI, STEP_OK, gCfg.kpi1Label + " " + String(s.kpi1) + " · " + gCfg.kpi2Label + " " + String(s.kpi2));
  if (s.kpi2 < 0 && gCfg.kpi2Jql.length()) {
    LOG("jira kpi2 failed: %s (keeping last)", err.c_str());
    if (prevReal) s.kpi2 = prevKpi2;
  }
  s.ok = true;
  s.updated = time(nullptr);
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

// Meeting place shown on the board, in this order:
//  1. a room code like "3M2" or "BM4" (letters/digits, "M", digits) from the location, title or description;
//  2. a Google Meet or Skyroom link from the location or description;
//  3. the location text as is.
static String findRoom(const String& s) {
  for (int i = 1; i + 1 < (int)s.length(); i++) {
    if (s[i] != 'M' || !isalnum((uint8_t)s[i - 1]) || !isdigit((uint8_t)s[i + 1])) continue;
    int a = i - 1, b = i + 1;
    while (a > 0 && isalnum((uint8_t)s[a - 1])) a--;
    while (b + 1 < (int)s.length() && isdigit((uint8_t)s[b + 1])) b++;
    return s.substring(a, b + 1);
  }
  return "";
}

static String findLink(const String& s) {
  for (const char* host : {"meet.google.com/", "skyroom.online/"}) {
    int at = s.indexOf(host);
    if (at < 0) continue;
    // Include a "www." in front; stop at whitespace, quotes, separators or an iCalendar escape.
    if (at >= 4 && s.substring(at - 4, at) == "www.") at -= 4;
    int end = at;
    while (end < (int)s.length() && !strchr(" \t\r\n\"'<>,;\\", s[end])) end++;
    return s.substring(at, end);
  }
  return "";
}

static String meetingPlace(const String& location, const String& title, const String& desc) {
  String room = findRoom(location);
  if (room.isEmpty()) room = findRoom(title);
  if (room.isEmpty()) room = findRoom(desc);
  if (room.length()) return room;
  String link = findLink(location);
  if (link.isEmpty()) link = findLink(desc);
  if (link.length()) return link;
  return location;
}

static String icsDesc;  // DESCRIPTION of the event being parsed (only used for the place)

static void handleIcsLine(const String& line, MeetingData& d, Meeting& cur, bool& inEvent, bool& allDay, time_t dayStart, time_t dayEnd) {
  if (line == "BEGIN:VEVENT") { cur = Meeting(); icsDesc = ""; inEvent = true; allDay = false; return; }
  if (!inEvent) return;
  if (line == "END:VEVENT") {
    inEvent = false;
    if (!allDay && cur.start >= dayStart && cur.start < dayEnd) {
      if (!cur.end) cur.end = cur.start + 30 * 60;
      cur.where = meetingPlace(cur.where, cur.title, icsDesc).substring(0, 60);
      addMeeting(d, cur);
    }
    return;
  }
  // Name and value are split at the first ':' outside quotes: Outlook writes
  // DTSTART;TZID="(UTC+03:30) Tehran":20261024T140000 (the timezone name has its own colon).
  int colon = -1;
  bool quoted = false;
  for (int i = 0; i < (int)line.length(); i++) {
    if (line[i] == '"') quoted = !quoted;
    else if (line[i] == ':' && !quoted) { colon = i; break; }
  }
  if (colon < 0) return;
  String name = line.substring(0, colon);
  String value = line.substring(colon + 1);
  int semi = name.indexOf(';');
  if (semi >= 0) name = name.substring(0, semi);
  bool ad;
  if (name == "DTSTART") { cur.start = parseIcsDate(value, ad); allDay = ad; }
  else if (name == "DTEND") cur.end = parseIcsDate(value, ad);
  else if (name == "SUMMARY") { value.replace("\\,", ","); cur.title = value.substring(0, 80); }
  else if (name == "LOCATION") { value.replace("\\,", ","); cur.where = value.substring(0, 120); }
  else if (name == "DESCRIPTION") icsDesc = value.substring(0, 800);
}

static bool getJson(const String& url, const String& bearer, JsonDocument& doc, const JsonDocument* filter, String& err) {
  return getJsonUnlocked(url, bearer, doc, filter, err);
}

static void fetchCalendarUnlocked(MeetingData& d);

static void fetchCalendar(MeetingData& d) { fetchCalendarUnlocked(d); }

static void fetchCalendarUnlocked(MeetingData& d) {
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
  int code = timedGet(http);
  if (code != 200) {
    d.err = code > 0 ? "HTTP " + String(code) : String(http.errorToString(code));
    if (code < 0) {
      char tlsErr[100] = "";
      if (gCfg.icsUrl.startsWith("https")) tls.lastError(tlsErr, sizeof(tlsErr));
      if (*tlsErr) d.err += String(" / tls: ") + tlsErr;
      d.err += " / heap " + String(ESP.getFreeHeap() / 1024) + "k, block " + String(ESP.getMaxAllocHeap() / 1024) + "k";
    }
    http.end();
    return;
  }

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
    if (folded && pending.length()) { if (pending.length() < 800) pending += line; continue; }
    if (pending.length()) handleIcsLine(pending, d, cur, inEvent, allDay, dayStart, dayEnd);
    pending = line.substring(0, 800);
  }
  if (pending.length()) handleIcsLine(pending, d, cur, inEvent, allDay, dayStart, dayEnd);
  http.end();

  std::sort(d.m, d.m + d.count, [](const Meeting& a, const Meeting& b) { return a.start < b.start; });
  d.ok = true;
  d.updated = now;
}

// ---------- refresh ----------

static bool timeValid() { return time(nullptr) > 1700000000; }

// Fills a fresh copy and swaps it in; on failure keeps the last good data and only records the error.
template <typename T, typename Fn>
static bool publish(const char* name, T Snapshot::*field, Fn fill, String& err) {
  LOG("fetch %s ...", name);
  uint32_t t0 = millis();
  T* fresh = new T();
  fill(*fresh);
  bool ok = fresh->ok;
  err = fresh->err;
  LOG("fetch %s %s in %lu ms %s", name, ok ? "ok" : "FAILED", millis() - t0, err.c_str());
  model_lock();
  if (ok || !(gData.*field).ok) gData.*field = *fresh;
  else (gData.*field).err = fresh->err;
  gData.version++;
  model_unlock();
  delete fresh;
  return ok;
}

RefreshKind fetch_due() {
  if (WiFi.status() != WL_CONNECTED || !timeValid()) return REFRESH_NONE;
  if (refreshNow || millis() - lastFetch >= gCfg.refreshMin * 60000UL) return REFRESH_FULL;
  if (millis() - lastKuma >= gCfg.kumaMin * 60000UL) return REFRESH_KUMA;
  return REFRESH_NONE;
}

void fetch_all(FetchProgress p, RefreshKind kind) {
  progress = p;
  String err;
  bool full = kind == REFRESH_FULL;
  if (full) refreshNow = false;

  if (!full) {
    for (int i = STEP_CARDS; i <= STEP_KPI; i++) report((FetchStep)i, STEP_SKIP, "unchanged");
  } else if (settings_mock()) {
    Snapshot* m = new Snapshot();
    model_fill_mock(*m);
    model_lock();
    gData.sprint = m->sprint;
    gData.meet = m->meet;
    gData.mock = true;
    gData.version++;
    model_unlock();
    delete m;
    for (int i = STEP_CARDS; i <= STEP_KPI; i++) report((FetchStep)i, STEP_OK, "demo data");
  } else {
    if (gData.mock) {
      // Leaving demo mode: drop the demo data so a failing first fetch shows its error, not demo cards.
      model_lock();
      gData.sprint = SprintData();
      gData.meet = MeetingData();
      gData.mock = false;
      model_unlock();
    }
    publish("jira", &Snapshot::sprint, fetchJira, err);
  }

  report(STEP_KUMA, STEP_RUN);
  bool ok = publish("kuma", &Snapshot::health, fetchKuma, err);
  model_lock();
  String kumaSum = String(gData.health.up) + " / " + String(gData.health.count) + " up";
  model_unlock();
  report(STEP_KUMA, ok ? STEP_OK : STEP_FAIL, ok ? kumaSum : err);

  if (!full) {
    report(STEP_CALENDAR, STEP_SKIP, "unchanged");
  } else if (settings_mock()) {
    report(STEP_CALENDAR, STEP_OK, "demo data");
  } else if (gCfg.icsUrl.isEmpty()) {
    report(STEP_CALENDAR, STEP_SKIP, "no .ics URL");
  } else {
    report(STEP_CALENDAR, STEP_RUN);
    ok = publish("calendar", &Snapshot::meet, fetchCalendar, err);
    model_lock();
    String calSum = String(gData.meet.count) + " meetings today";
    model_unlock();
    report(STEP_CALENDAR, ok ? STEP_OK : STEP_FAIL, ok ? calSum : err);
  }
  if (full) lastFetch = millis();
  lastKuma = millis();
  progress = nullptr;
}
