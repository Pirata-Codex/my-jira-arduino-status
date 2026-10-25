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
static int lastFetchYday = -1;  // local day of the last full fetch: the calendar holds today only

static int localYday() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  return lt.tm_yday;
}
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
  // The issue list is the biggest JSON here: keep it only inside this block, so it is freed
  // before the sprint and KPI calls need RAM for their own TLS handshakes.
  {
  JsonDocument doc;
  report(STEP_CARDS, STEP_RUN);
  bool got = getJson(url, gCfg.jiraPat, doc, &filter, s.err);
  filter.clear();
  if (!got) {
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
  }
  LOG("jira: cards parsed, issue list freed");

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

// Stream wrapper with one character of push-back, so the heartbeat reader can peek at the
// next token and still hand a complete "{...}" to ArduinoJson.
class PeekStream : public Stream {
 public:
  explicit PeekStream(Stream& s) : s_(s) {}
  int read() override {
    if (pending_ >= 0) { int c = pending_; pending_ = -1; return c; }
    char c;
    return s_.readBytes(&c, 1) == 1 ? (uint8_t)c : -1;  // blocking, uses the stream timeout
  }
  int peek() override {
    if (pending_ < 0) pending_ = read();
    return pending_;
  }
  int available() override { return (pending_ >= 0 ? 1 : 0) + s_.available(); }
  size_t write(uint8_t) override { return 0; }
  int skipSpace() {
    while (peek() >= 0 && isspace(peek())) read();
    return peek();
  }

 private:
  Stream& s_;
  int pending_ = -1;
};

struct Beat { int id = -1; bool up = true; int ping = -1; };

// /api/status-page/heartbeat/<slug> holds ~100 beats per monitor (~40 KB). Building it as one JSON
// document ran out of memory, so it is read beat by beat and only the newest beat per monitor is kept.
static bool readHeartbeats(Stream& in, Beat* beats, int& n, int maxN, JsonDocument& uptime, String& err) {
  n = 0;
  in.setTimeout(10000);
  if (!in.find((char*)"\"heartbeatList\":{")) { err = "kuma: no heartbeatList"; return false; }
  PeekStream ps(in);
  JsonDocument beatFilter;
  beatFilter["status"] = true;
  beatFilter["ping"] = true;
  JsonDocument beat;
  for (;;) {
    int c = ps.skipSpace();
    if (c == '}') { ps.read(); break; }
    if (c == ',') { ps.read(); continue; }
    if (c != '"') { err = "kuma: bad heartbeat list"; return false; }
    ps.read();
    String id = ps.readStringUntil('"');
    if (ps.skipSpace() != ':') { err = "kuma: bad heartbeat list"; return false; }
    ps.read();
    if (ps.skipSpace() != '[') { err = "kuma: bad heartbeat list"; return false; }
    ps.read();
    Beat* b = n < maxN ? &beats[n++] : nullptr;
    if (b) { *b = Beat(); b->id = id.toInt(); }
    for (;;) {
      int d = ps.skipSpace();
      if (d == ']') { ps.read(); break; }
      if (d == ',') { ps.read(); continue; }
      if (d != '{') { err = "kuma: bad beat"; return false; }
      DeserializationError e = deserializeJson(beat, ps, DeserializationOption::Filter(beatFilter));
      if (e) { err = String("kuma beat: ") + e.c_str(); return false; }
      if (b) {  // oldest first: the last one wins
        int status = beat["status"] | 1;  // 0 down, 1 up, 2 pending, 3 maintenance
        b->up = status != 0;
        b->ping = beat["ping"] | -1;
      }
    }
  }
  // "uptimeList":{"74_24":1,...} is small; read it as one object.
  if (ps.find((char*)"\"uptimeList\":")) {
    DeserializationError e = deserializeJson(uptime, ps);
    if (e) LOG("kuma: uptime list: %s", e.c_str());
  }
  return true;
}

static void fetchKuma(HealthData& h) {
  h = HealthData();
  if (gCfg.kumaUrl.isEmpty() || gCfg.kumaSlug.isEmpty()) { h.err = "not configured"; return; }
  String base = trimSlash(gCfg.kumaUrl);

  // 1) Monitor ids and names from the status page (small); copied out so the JSON is freed at once.
  int ids[MAX_MONITORS];
  {
    JsonDocument f1;
    JsonObject mf = f1["publicGroupList"][0]["monitorList"][0].to<JsonObject>();
    mf["id"] = true;
    mf["name"] = true;
    JsonDocument page;
    if (!getJson(base + "/api/status-page/" + gCfg.kumaSlug, "", page, &f1, h.err)) return;
    for (JsonObject group : page["publicGroupList"].as<JsonArray>())
      for (JsonObject mon : group["monitorList"].as<JsonArray>()) {
        if (h.count >= MAX_MONITORS) break;
        ids[h.count] = mon["id"] | -1;
        h.m[h.count++].name = mon["name"].as<const char*>();
      }
  }

  // 2) Latest beat per monitor, streamed.
  HTTPClient http;
  WiFiClientSecure tls;
  WiFiClient plain;
  String url = base + "/api/status-page/heartbeat/" + gCfg.kumaSlug;
  if (!begin(http, tls, plain, url)) { h.err = "bad url"; return; }
  int code = timedGet(http);
  if (code != 200) {
    h.err = code > 0 ? "HTTP " + String(code) : String(http.errorToString(code));
    http.end();
    return;
  }
  static Beat beats[16];
  int nBeats = 0;
  JsonDocument uptime;
  bool ok = readHeartbeats(http.getStream(), beats, nBeats, 16, uptime, h.err);
  http.end();
  if (!ok) return;

  for (int i = 0; i < h.count; i++) {
    Monitor& m = h.m[i];
    for (int j = 0; j < nBeats; j++)
      if (beats[j].id == ids[i]) { m.up = beats[j].up; m.ping = beats[j].ping; }
    m.uptime = uptime[String(ids[i]) + "_24"] | -1.0f;
    if (m.up) h.up++;
  }
  h.ok = true;
  h.updated = time(nullptr);
}

// ---------- Calendar

// "https://host[:port]/path" -> parts.
static bool splitUrl(const String& url, bool& https, String& host, uint16_t& port, String& path) {
  https = url.startsWith("https://");
  if (!https && !url.startsWith("http://")) return false;
  int hs = https ? 8 : 7;
  int ps = url.indexOf('/', hs);
  String hp = ps < 0 ? url.substring(hs) : url.substring(hs, ps);
  path = ps < 0 ? "/" : url.substring(ps);
  int c = hp.indexOf(':');
  host = c < 0 ? hp : hp.substring(0, c);
  port = c < 0 ? (https ? 443 : 80) : hp.substring(c + 1).toInt();
  return host.length() > 0;
}

// Reads an HTTP body, undoing "Transfer-Encoding: chunked" when the server uses it.
// done() turns true at the end of the body or when the connection closes.
class ChunkedStream : public Stream {
 public:
  explicit ChunkedStream(Stream& s, bool chunked = false) : s_(&s), chunked_(chunked) {}
  int read() override {
    if (done_) return -1;
    if (chunked_ && left_ == 0) {
      if (started_) s_->readStringUntil('\n');  // CRLF after the previous chunk
      started_ = true;
      String size = s_->readStringUntil('\n');
      size.trim();
      if (!size.length()) { done_ = true; return -1; }
      left_ = strtol(size.c_str(), nullptr, 16);
      if (left_ == 0) { done_ = true; return -1; }
    }
    // Wait for the byte ourselves: the TLS client's readBytes() gives up as soon as the next TLS
    // record hasn't arrived yet, which cut the calendar off after its first 16 KB record.
    uint32_t t0 = millis();
    for (;;) {
      int c = s_->read();
      if (c >= 0) {
        if (chunked_) left_--;
        return c;
      }
      Client* cl = static_cast<Client*>(s_);
      if (!cl->connected() && !cl->available()) break;
      if (millis() - t0 >= getTimeout()) break;
      delay(2);
    }
    done_ = true;
    return -1;
  }
  int peek() override { return -1; }
  int available() override { return done_ ? 0 : 1; }
  size_t write(uint8_t) override { return 0; }
  bool done() const { return done_; }
  // Stream::readStringUntil waits out the whole timeout at the end of the body; this stops at once.
  String readLine() {
    String l;
    for (int c = read(); c >= 0 && c != '\n'; c = read()) l += (char)c;
    return l;
  }

 private:
  Stream* s_;
  bool chunked_;
  bool started_ = false;
  bool done_ = false;
  long left_ = 0;
};

// Adds m to list (count of max); when full, it replaces the latest one if it starts earlier.
static void addTo(Meeting* list, int& count, int max, const Meeting& m) {
  if (count < max) { list[count++] = m; return; }
  int latest = 0;
  for (int i = 1; i < count; i++) if (list[i].start > list[latest].start) latest = i;
  if (m.start < list[latest].start) list[latest] = m;
}

static void addMeeting(MeetingData& d, const Meeting& m) { addTo(d.m, d.count, MAX_MEETINGS, m); }

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

static String hhmmLog(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  char b[8];
  strftime(b, sizeof(b), "%H:%M", &lt);
  return b;
}

static String icsDesc;  // DESCRIPTION of the event being parsed (only used for the place)

// Repeating events. Outlook writes one VEVENT with an RRULE for the series, EXDATEs for deleted
// occurrences, and a separate VEVENT with RECURRENCE-ID for each changed (moved, renamed or
// cancelled) occurrence. Only today matters, so the series is expanded for today alone.
static String icsRule;
static time_t icsRecId;
static bool icsCancelled;
static uint32_t icsUid;
constexpr int MAX_EXDATES = 24;
static time_t icsExdates[MAX_EXDATES];
static int icsExCount;
// Today's occurrences replaced by a RECURRENCE-ID event: (uid, original start).
constexpr int MAX_OVERRIDES = 32;
static uint32_t ovUid[MAX_OVERRIDES];
static time_t ovStart[MAX_OVERRIDES];
static int ovCount;

static uint32_t fnv1a(const String& s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
  return h;
}

static String ruleField(const String& rule, const char* key) {
  String k = String(key) + "=";
  int i = rule.startsWith(k) ? 0 : rule.indexOf(";" + k);
  if (i < 0) return "";
  if (i > 0) i++;
  int e = rule.indexOf(';', i);
  return rule.substring(i + k.length(), e < 0 ? rule.length() : e);
}

// Local midnight of the day containing t (Iran has no DST, so days are 86400 s).
static time_t dayOf(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  return localEpoch(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, 0, 0, 0);
}

static int weekdayOf(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  return lt.tm_wday;  // 0 = Sunday
}

static int weekdayCode(const String& s) {
  static const char* codes[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
  String c = s.substring(max(0, (int)s.length() - 2));  // "1MO" (monthly form) -> "MO"
  for (int i = 0; i < 7; i++) if (c == codes[i]) return i;
  return -1;
}

// Start of today's occurrence of the series starting at dtStart, or 0 if it has none today.
// Handles FREQ=DAILY and WEEKLY with INTERVAL, BYDAY, UNTIL, COUNT and WKST (what Outlook writes for
// meetings); other frequencies are skipped.
static time_t occurrenceToday(const String& rule, time_t dtStart, time_t dayStart) {
  String freq = ruleField(rule, "FREQ");
  bool daily = freq == "DAILY", weekly = freq == "WEEKLY";
  if (!daily && !weekly) return 0;
  int interval = max(1, (int)ruleField(rule, "INTERVAL").toInt());
  int count = ruleField(rule, "COUNT").toInt();
  String untilS = ruleField(rule, "UNTIL");
  bool ad;
  time_t until = untilS.length() ? parseIcsDate(untilS, ad) : 0;
  if (until && ad) until += 86399;  // UNTIL as a date includes that whole day
  bool days[7] = {};
  bool anyDay = false;
  String by = ruleField(rule, "BYDAY");
  for (int i = 0; i < (int)by.length();) {
    int e = by.indexOf(',', i);
    if (e < 0) e = by.length();
    int w = weekdayCode(by.substring(i, e));
    if (w >= 0) { days[w] = true; anyDay = true; }
    i = e + 1;
  }
  if (weekly && !anyDay) { days[weekdayOf(dtStart)] = true; anyDay = true; }
  int wkst = weekdayCode(ruleField(rule, "WKST"));
  if (wkst < 0) wkst = 1;  // RFC 5545 default: Monday

  time_t startDay = dayOf(dtStart);
  if (dayStart < startDay) return 0;
  time_t tod = dtStart - startDay;
  long lastDay = (dayStart - startDay) / 86400;
  if (!count) {
    // No COUNT: only today needs checking.
    long n = lastDay;
    time_t day = startDay + n * 86400;
    int wd = weekdayOf(day);
    if (anyDay && !days[wd]) return 0;
    if (daily && n % interval) return 0;
    if (weekly && interval > 1) {
      time_t w0 = startDay - ((weekdayOf(startDay) - wkst + 7) % 7) * 86400L;
      time_t w1 = day - ((wd - wkst + 7) % 7) * 86400L;
      if (((w1 - w0) / (7 * 86400L)) % interval) return 0;
    }
    time_t occ = day + tod;
    if (occ < dtStart || (until && occ > until)) return 0;
    return occ;
  }
  // COUNT: walk the days from the start and count the occurrences up to today.
  int seen = 0;
  time_t w0 = startDay - ((weekdayOf(startDay) - wkst + 7) % 7) * 86400L;
  for (long n = 0; n <= lastDay && n < 3700; n++) {
    time_t day = startDay + n * 86400;
    int wd = weekdayOf(day);
    if (anyDay && !days[wd]) continue;
    if (daily && n % interval) continue;
    if (weekly && interval > 1) {
      time_t w1 = day - ((wd - wkst + 7) % 7) * 86400L;
      if (((w1 - w0) / (7 * 86400L)) % interval) continue;
    }
    time_t occ = day + tod;
    if (occ < dtStart) continue;
    if (++seen > count || (until && occ > until)) return 0;
    if (n == lastDay) return occ;
  }
  return 0;
}

static void parseExdates(const String& value) {
  bool ad;
  for (int i = 0; i < (int)value.length();) {
    int e = value.indexOf(',', i);
    if (e < 0) e = value.length();
    if (icsExCount < MAX_EXDATES) icsExdates[icsExCount++] = parseIcsDate(value.substring(i, e), ad);
    i = e + 1;
  }
}

// After the whole file: drop rule occurrences that a RECURRENCE-ID event replaced or cancelled.
static void dropOverridden(Meeting* list, int& count) {
  for (int o = 0; o < ovCount; o++)
    for (int i = 0; i < count; i++)
      if (list[i].fromRule && list[i].uid == ovUid[o] && list[i].start == ovStart[o]) {
        for (int j = i; j < count - 1; j++) list[j] = list[j + 1];
        list[--count] = Meeting();
        break;
      }
}

static void applyOverrides(MeetingData& d) {
  dropOverridden(d.m, d.count);
  dropOverridden(d.future, d.futureCount);
}

// Files a meeting under today or the upcoming list (or nowhere, when outside the window).
static void placeMeeting(MeetingData& d, const Meeting& m, time_t dayStart, time_t dayEnd) {
  if (m.start >= dayStart && m.start < dayEnd) addMeeting(d, m);
  else if (m.start >= dayEnd && m.start < dayEnd + AGENDA_AHEAD_DAYS * 86400L)
    addTo(d.future, d.futureCount, MAX_FUTURE, m);
}

static void handleIcsLine(const String& line, MeetingData& d, Meeting& cur, bool& inEvent, bool& allDay, time_t dayStart, time_t dayEnd) {
  if (line == "BEGIN:VEVENT") {
    cur = Meeting();
    icsDesc = icsRule = "";
    icsRecId = 0;
    icsCancelled = false;
    icsUid = 0;
    icsExCount = 0;
    inEvent = true;
    allDay = false;
    return;
  }
  if (!inEvent) return;
  if (line == "END:VEVENT") {
    inEvent = false;
    if (allDay) return;
    time_t dur = cur.end > cur.start ? cur.end - cur.start : 30 * 60;
    time_t windowEnd = dayEnd + AGENDA_AHEAD_DAYS * 86400L;
    if (icsRecId) {
      // A changed occurrence: it replaces the series' occurrence at icsRecId.
      if (icsRecId >= dayStart && icsRecId < windowEnd && ovCount < MAX_OVERRIDES) {
        ovUid[ovCount] = icsUid;
        ovStart[ovCount++] = icsRecId;
      }
    }
    if (icsCancelled) return;
    cur.uid = icsUid;
    cur.where = meetingPlace(cur.where, cur.title, icsDesc).substring(0, 60);
    if (icsRule.length() && !icsRecId) {
      // A series: today's occurrence and those of the next days (Iran has no DST: days are 86400 s).
      time_t first = cur.start;
      cur.fromRule = true;
      for (int k = 0; k <= AGENDA_AHEAD_DAYS; k++) {
        time_t occ = occurrenceToday(icsRule, first, dayStart + k * 86400L);
        for (int i = 0; occ && i < icsExCount; i++)
          if (icsExdates[i] == occ) occ = 0;
        if (!occ) continue;
        cur.start = occ;
        cur.end = occ + dur;
        placeMeeting(d, cur, dayStart, dayEnd);
      }
      return;
    }
    cur.end = cur.start + dur;
    placeMeeting(d, cur, dayStart, dayEnd);
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
  else if (name == "RRULE") icsRule = value;
  else if (name == "EXDATE") parseExdates(value);
  else if (name == "RECURRENCE-ID") icsRecId = parseIcsDate(value, ad);
  else if (name == "UID") icsUid = fnv1a(value);
  else if (name == "STATUS") icsCancelled = value == "CANCELLED";
}

static bool getJson(const String& url, const String& bearer, JsonDocument& doc, const JsonDocument* filter, String& err) {
  return getJsonUnlocked(url, bearer, doc, filter, err);
}

static void fetchCalendarUnlocked(MeetingData& d, const String& url0);

// The same link with the host swapped for the mail server's address inside the company, over plain
// http. From outside the mail server's public address answers the ESP32 with 403; inside the company
// network the internal server serves the same file to any client.
static String lanIcsUrl() {
  if (!strlen(ICS_LAN_HOST)) return "";
  const String& u = gCfg.icsUrl;
  int hs = u.indexOf("://");
  if (hs < 0) return "";
  int ps = u.indexOf('/', hs + 3);
  return ps < 0 ? "" : String("http://") + ICS_LAN_HOST + u.substring(ps);
}

// Tries the configured link up to 3 times (the public server fails intermittently). On the company
// network (a 10.x address) the internal server is tried last; elsewhere it can't be reached.
static void fetchCalendar(MeetingData& d) {
  String link = gCfg.icsUrl;
  link.trim();  // a stray space or newline from pasting ends up in the request path
  String lan = WiFi.localIP()[0] == 10 ? lanIcsUrl() : String();
  MeetingData first;
  int tries = lan.length() ? 4 : 3;
  for (int attempt = 1; attempt <= tries; attempt++) {
    bool useLan = attempt == 4;
    fetchCalendarUnlocked(d, useLan ? lan : link);
    if (d.ok || !d.configured) {
      if (d.ok) LOG("calendar: ok via %s", useLan ? "internal server" : "configured link");
      return;
    }
    if (attempt == 1) first.err = d.err;
    if (attempt < tries) {
      LOG("calendar: %s, retry %d/%d", d.err.c_str(), attempt, tries - 1);
      delay(1000 * attempt);
    }
  }
  if (lan.length()) d.err = first.err + " (internal: " + d.err + ")";
}

static void fetchCalendarUnlocked(MeetingData& d, const String& url0) {
  d = MeetingData();
  if (gCfg.icsUrl.isEmpty()) return;
  d.configured = true;

  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  time_t dayStart = localEpoch(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, 0, 0, 0);
  time_t dayEnd = dayStart + 86400;

  // The request is written by hand: curl gets 200 from this server where HTTPClient got 403, and
  // HTTPClient always adds headers of its own (Accept-Encoding, Connection, ...). This sends exactly
  // what curl sends.
  WiFiClientSecure tls;
  WiFiClient plain;
  tls.setInsecure();
  // curl announces ALPN and the ESP32 does not by default; the mail server's firewall may reject
  // TLS clients without it, so announce plain HTTP/1.1 like curl --http1.1 does.
  static const char* alpn[] = {"http/1.1", nullptr};
  tls.setAlpnProtocols(alpn);
  String url = url0;
  Client* net = nullptr;
  ChunkedStream body(tls);
  String headers, server;  // of the last response, logged on failure (they name the firewall)
  int code = 0;
  int total = -1;  // Content-Length, when the server sends one
  for (int hop = 0; hop < 3; hop++) {  // follow up to 2 redirects
    String host, path;
    uint16_t port;
    bool https;
    if (!splitUrl(url, https, host, port, path)) { d.err = "bad url"; return; }
    net = https ? (Client*)&tls : (Client*)&plain;
    net->stop();
    if (!net->connect(host.c_str(), port)) {
      d.err = "connect failed";
      if (https) {
        char tlsErr[100] = "";
        tls.lastError(tlsErr, sizeof(tlsErr));
        if (*tlsErr) d.err += String(" / tls: ") + tlsErr;
      }
      d.err += " / heap " + String(ESP.getFreeHeap() / 1024) + "k, block " + String(ESP.getMaxAllocHeap() / 1024) + "k";
      return;
    }
    net->print("GET " + path + " HTTP/1.1\r\nHost: " + host +
               "\r\nUser-Agent: curl/8.9.1\r\nAccept: */*\r\nConnection: close\r\n\r\n");
    Stream& in = *net;
    in.setTimeout(20000);
    String status = in.readStringUntil('\n');  // "HTTP/1.1 200 OK"
    code = status.substring(status.indexOf(' ') + 1).toInt();
    String location;
    headers = server = "";
    bool chunked = false;
    total = -1;
    for (;;) {
      String h = in.readStringUntil('\n');
      h.trim();
      if (!h.length()) break;
      String lower = h;
      lower.toLowerCase();
      if (lower.startsWith("location:")) location = h.substring(9);
      if (lower.startsWith("content-length:")) total = h.substring(15).toInt();
      if (lower.startsWith("server:")) { server = h.substring(7); server.trim(); }
      if (headers.length() < 600) headers += h + " | ";
      if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") > 0) chunked = true;
    }
    location.trim();
    LOG("  GET %s -> %d%s (path %u chars, ends \"%s\")", host.c_str(), code, chunked ? " (chunked)" : "",
        (unsigned)path.length(), path.substring(max(0, (int)path.length() - 16)).c_str());
    body = ChunkedStream(*net, chunked);
    if (code >= 300 && code < 400 && location.length()) {
      url = location.startsWith("http") ? location : String(https ? "https://" : "http://") + host + location;
      continue;
    }
    break;
  }
  if (code != 200) {
    // Show what the server says (a 403 page usually names the reason) on the screen, not only in the log.
    String page;
    while (page.length() < 400) { int c = body.read(); if (c < 0) break; page += (char)c; }
    net->stop();
    int t = page.indexOf("<title>");
    String why = t >= 0 ? page.substring(t + 7, page.indexOf("</title>", t)) : page;
    why.replace("\r", "");
    why.replace("\n", " ");
    why.trim();
    LOG("calendar: HTTP %d, server says: %s", code, page.c_str());
    LOG("calendar: response headers: %s", headers.c_str());
    d.err = code > 0 ? "HTTP " + String(code) : String("no response");
    if (server.length()) d.err += " [" + server + "]";
    if (why.length()) d.err += ": " + why.substring(0, 60);
    return;
  }

  Meeting cur;
  bool inEvent = false, allDay = false;
  ovCount = 0;
  String pending;
  size_t bytesRead = 0;
  uint32_t t0 = millis();
  // Outlook lists events oldest first, so today's are near the end: read the whole file.
  uint32_t deadline = t0 + 90000;
  body.setTimeout(15000);
  while (millis() < deadline) {
    String line = body.readLine();
    if (!line.length() && body.done()) break;
    bytesRead += line.length() + 1;
    // Folded lines continue the previous one with a leading space.
    bool folded = line.length() && (line[0] == ' ' || line[0] == '\t');
    line.trim();
    if (folded && pending.length()) { if (pending.length() < 800) pending += line; continue; }
    if (pending.length()) handleIcsLine(pending, d, cur, inEvent, allDay, dayStart, dayEnd);
    pending = line.substring(0, 800);
  }
  if (pending.length()) handleIcsLine(pending, d, cur, inEvent, allDay, dayStart, dayEnd);
  applyOverrides(d);
  net->stop();
  bool timedOut = millis() >= deadline;
  LOG("calendar: read %u of %d bytes in %lu ms%s, %d meetings today", (unsigned)bytesRead, total,
      millis() - t0, timedOut ? " (TIMED OUT)" : "", d.count);

  auto byStart = [](const Meeting& a, const Meeting& b) { return a.start < b.start; };
  std::sort(d.m, d.m + d.count, byStart);
  std::sort(d.future, d.future + d.futureCount, byStart);
  for (int i = 0; i < d.count; i++)
    LOG("  meeting %s-%s %s @ %s", hhmmLog(d.m[i].start).c_str(), hhmmLog(d.m[i].end).c_str(),
        d.m[i].title.c_str(), d.m[i].where.c_str());
  for (int i = 0; i < d.futureCount; i++) {
    struct tm ft;
    localtime_r(&d.future[i].start, &ft);
    LOG("  upcoming %02d/%02d %s %s", ft.tm_mon + 1, ft.tm_mday, hhmmLog(d.future[i].start).c_str(), d.future[i].title.c_str());
  }
  if (timedOut) {
    // A cut-off file would silently miss today's events (they are at the end); report it instead.
    d.err = "timed out after " + String(bytesRead / 1024) + " KB";
    return;
  }
  if (total > 0 && bytesRead < (size_t)total) {
    // The connection dropped mid-file. Today's events are at the end, so this would show a
    // shortened day as if it were complete: fail instead, so the next try reads it again.
    char tlsErr[100] = "";
    tls.lastError(tlsErr, sizeof(tlsErr));
    LOG("calendar: cut off at %u of %d bytes, tls: %s", (unsigned)bytesRead, total, tlsErr);
    d.err = "cut off at " + String(bytesRead / 1024) + " of " + String(total / 1024) + " KB";
    return;
  }
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
  if (lastFetchYday >= 0 && localYday() != lastFetchYday) return REFRESH_FULL;  // just past midnight
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
    // Calendar first, while the heap is least fragmented: the mail server's TLS handshake is the heaviest.
    if (gCfg.icsUrl.isEmpty()) {
      report(STEP_CALENDAR, STEP_SKIP, "no .ics URL");
    } else {
      report(STEP_CALENDAR, STEP_RUN);
      bool calOk = publish("calendar", &Snapshot::meet, fetchCalendar, err);
      model_lock();
      String calSum = String(gData.meet.count) + " meetings today";
      model_unlock();
      report(STEP_CALENDAR, calOk ? STEP_OK : STEP_FAIL, calOk ? calSum : err);
    }
    publish("jira", &Snapshot::sprint, fetchJira, err);
  }

  report(STEP_KUMA, STEP_RUN);
  bool ok = publish("kuma", &Snapshot::health, fetchKuma, err);
  model_lock();
  String kumaSum = String(gData.health.up) + " / " + String(gData.health.count) + " up";
  model_unlock();
  report(STEP_KUMA, ok ? STEP_OK : STEP_FAIL, ok ? kumaSum : err);

  if (!full) report(STEP_CALENDAR, STEP_SKIP, "unchanged");
  else if (settings_mock()) report(STEP_CALENDAR, STEP_OK, "demo data");
  if (full) {
    lastFetch = millis();
    lastFetchYday = localYday();
  }
  lastKuma = millis();
  progress = nullptr;
}
