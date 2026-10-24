#include "ui.h"
#include "config.h"
#include "theme.h"
#include "model.h"
#include "settings.h"
#include <WiFi.h>
#include "fetch.h"

// F.R.I.D.A.Y. HUD look. Behavior (pages, rotation, refresh flow, alerts) is unchanged; drawing is kept
// cheap for the ILI9488 (~10 full frames/s): outline-only panels, no shadows, only small areas animate.

enum Page { PG_NOW, PG_AGENDA, PG_SPRINT, PG_CARDS, PG_KPI, PG_HEALTH, PG_COUNT };
static const char* PAGE_TITLES[PG_COUNT] = {"PULSE", "AGENDA", "SPRINT", "CARDS", "KPI", "HEALTH"};
static const char* PAGE_ICONS[PG_COUNT] = {LV_SYMBOL_HOME, LV_SYMBOL_BELL, LV_SYMBOL_REFRESH, LV_SYMBOL_LIST, LV_SYMBOL_CHARGE, LV_SYMBOL_POWER};
static const int TAB_W = 472 / PG_COUNT;

static Snapshot snap;                 // UI-side copy of gData
static uint32_t seenVersion = UINT32_MAX;
static lv_obj_t* screens[PG_COUNT];
static lv_obj_t* bootScreen;
static lv_obj_t *bootTitle, *bootDetail;
static int page = PG_NOW;
static uint32_t lastSwitch = 0, lastClock = 0;
static bool started = false;

// header + tab bar (lv_layer_top, survive refreshes)
static lv_obj_t *header, *hdPhrase, *hdHealth, *hdClock, *hdWifi[4], *hdDot;
static lv_obj_t *tabBar, *tabLbl[PG_COUNT], *tabLine;
static String hdPhraseText;
// banner (meeting / outage)
static lv_obj_t *banner, *bnLine, *bnBar, *bnTag, *bnText, *bnRight, *bnCount, *bnCountLbl;
static int bannerMode = -1;
// Now
static lv_obj_t *nowClock, *nowDate, *nowJalali, *nowMeet, *nowTlBar, *nowTlNow, *nowSprint, *nowDays, *nowRows;
static lv_obj_t *nowLeg[4];
// Sprint
static lv_obj_t *spName, *spDays, *spBars[4], *spBarLbl[4], *spChart, *spFoot, *spBurnNote;
static lv_chart_series_t *spSeries, *spIdeal;
// List rows (cards, PULSE tickets, agenda) are fixed slots built once with their screen. Updates
// only change text/colors and show or hide slots, so page flips and the per-minute tick don't
// free and reallocate LVGL objects (that fragments the heap the HTTPS handshakes need).
struct TicketSlot { lv_obj_t *box, *bar, *key, *title, *pill, *pillLbl, *pts; };
struct MeetSlot { lv_obj_t *box, *outline, *bar, *start, *end, *title, *where, *pill, *pillLbl; };
constexpr int MAX_CARD_ROWS = 5, MAX_AGENDA_ROWS = 4;
// Agenda
static lv_obj_t *agList, *agMsg;
static MeetSlot agRows[MAX_AGENDA_ROWS];
static int agLayout = -1;  // number of rows the slots are currently sized for
// Cards
static lv_obj_t *cardList, *cardMsg;
static TicketSlot cardRows[MAX_CARD_ROWS];
static int cardRowsBuilt = 0;  // slots are created on first use, so unused ones cost no RAM
static TicketSlot nowTickets[2];
static lv_obj_t* nowMsg;
static uint32_t cardsDwellMs = 0;  // how long the cards slide needs to show every page once
// KPIs
static lv_obj_t *kpiVal[6], *kpiLbl[6], *kpiSub[6];
// Health
static lv_obj_t *hlSummary, *hlGrid, *hlMsg;
struct HealthSlot { lv_obj_t *box, *outline, *dot, *name, *sub, *track, *fill; };
constexpr int MAX_HEALTH_TILES = 8;
static HealthSlot hlTiles[MAX_HEALTH_TILES];
static int hlTilesBuilt = 0;

static const int CONTENT_Y = 40;      // between the header (0..36) and the tab bar (268..316)

// ---------- small builders ----------

static lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* s = "") {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, s);
  return l;
}

static lv_obj_t* hudText(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* s = "") {
  lv_obj_t* l = text(parent, font, color, s);
  lv_obj_set_style_text_letter_space(l, 1, 0);
  return l;
}

static lv_obj_t* rect(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t color, int radius = 0) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, color, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t* plain(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

// lv_line keeps a pointer to its points, so each line owns a heap copy, freed with the line.
static void freePoints(lv_event_t* e) { lv_free(lv_event_get_user_data(e)); }

static lv_obj_t* polyline(lv_obj_t* parent, const lv_point_precise_t* src, int n, lv_color_t color, int width) {
  auto* pts = (lv_point_precise_t*)lv_malloc(sizeof(lv_point_precise_t) * n);
  memcpy(pts, src, sizeof(lv_point_precise_t) * n);
  lv_obj_t* l = lv_line_create(parent);
  lv_line_set_points(l, pts, n);
  lv_obj_set_style_line_color(l, color, 0);
  lv_obj_set_style_line_width(l, width, 0);
  lv_obj_add_event_cb(l, freePoints, LV_EVENT_DELETE, pts);
  lv_obj_set_pos(l, 0, 0);
  return l;
}

// HUD panel: transparent box with a chamfered outline; optional bright corner brackets.
// Children are placed with absolute coordinates inside it.
static lv_obj_t* panel(lv_obj_t* parent, int x, int y, int w, int h, int cut = 8, lv_color_t line = C_LINE,
                       bool brackets = false, lv_obj_t** outline = nullptr) {
  lv_obj_t* o = plain(parent, x, y, w, h);
  int r = w - 1, b = h - 1;
  lv_point_precise_t p[9] = {{cut, 0}, {r - cut, 0}, {r, cut}, {r, b - cut}, {r - cut, b},
                             {cut, b}, {0, b - cut}, {0, cut}, {cut, 0}};
  lv_obj_t* l = polyline(o, p, 9, line, 1);
  if (outline) *outline = l;
  if (brackets) {
    lv_point_precise_t tl[4] = {{0, cut + 14}, {0, cut}, {cut, 0}, {cut + 14, 0}};
    lv_point_precise_t br[4] = {{r, b - cut - 14}, {r, b - cut}, {r - cut, b}, {r - cut - 14, b}};
    polyline(o, tl, 4, C_CYAN, 2);
    polyline(o, br, 4, C_CYAN, 2);
  }
  return o;
}

static lv_obj_t* newScreen() {
  lv_obj_t* s = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s, C_BG, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

// Long messages (fetch errors) wrap instead of being cut off.
static lv_obj_t* errorText(lv_obj_t* parent, int x, int y, const char* s) {
  lv_obj_t* l = hudText(parent, F_HUD, C_AMBER, s);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_width(l, 440);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  return l;
}

// "Software Dev-Sprint-1405-73" -> "SPRINT 1405-73" (pattern 14dd-n); other names unchanged.
static String sprintShort(const String& name) {
  for (int i = 0; i + 5 < (int)name.length(); i++) {
    if (name[i] != '1' || name[i + 1] != '4' || !isdigit((uint8_t)name[i + 2]) || !isdigit((uint8_t)name[i + 3]) ||
        name[i + 4] != '-' || !isdigit((uint8_t)name[i + 5]))
      continue;
    int j = i + 5;
    while (j < (int)name.length() && isdigit((uint8_t)name[j])) j++;
    return "SPRINT " + name.substring(i, j);
  }
  return name;
}

static lv_color_t catColor(CardCat c, bool blocked) {
  if (blocked) return C_AMBER;
  switch (c) {
    case CAT_PROGRESS: return C_CYAN;
    case CAT_REVIEW: return C_VIOLET;
    case CAT_DONE: return C_LIME;
    default: return C_ICE;
  }
}

static const char* catName(CardCat c, bool blocked) {
  static const char* n[] = {"TO DO", "IN PROGRESS", "REVIEW", "DONE"};
  return blocked ? "BLOCKED" : n[c];
}

static String fmtPts(float p) {
  return (p == (int)p) ? String((int)p) : String(p, 1);
}

static String hhmm(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  char b[8];
  strftime(b, sizeof(b), "%H:%M", &lt);
  return b;
}

static String mmss(long sec) {
  char b[12];
  snprintf(b, sizeof(b), "%02ld:%02ld", sec / 60, sec % 60);
  return b;
}

// Persian (Jalali) date as 1405/08/02, in the same HUD digits as the Gregorian date.
static String jalaliDate(const struct tm& lt) {
  static const int gdm[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int gy = lt.tm_year + 1900, gm = lt.tm_mon + 1, gd = lt.tm_mday;
  int gy2 = gm > 2 ? gy + 1 : gy;
  long days = 355666L + 365L * gy + (gy2 + 3) / 4 - (gy2 + 99) / 100 + (gy2 + 399) / 400 + gd + gdm[gm - 1];
  int jy = -1595 + 33 * (int)(days / 12053);
  days %= 12053;
  jy += 4 * (int)(days / 1461);
  days %= 1461;
  if (days > 365) {
    jy += (int)((days - 1) / 365);
    days = (days - 1) % 365;
  }
  int jm, jd;
  if (days < 186) { jm = 1 + days / 31; jd = 1 + days % 31; }
  else { jm = 7 + (days - 186) / 30; jd = 1 + (days - 186) % 30; }
  char b[12];
  snprintf(b, sizeof(b), "%04d/%02d/%02d", jy, jm, jd);
  return b;
}

// ---------- arc reactor + segmented ring ----------

static void spinCb(void* arc, int32_t v) { lv_arc_set_rotation((lv_obj_t*)arc, v); }

static lv_obj_t* arcRing(lv_obj_t* parent, int x, int y, int size, int width, lv_color_t color, bool track) {
  lv_obj_t* a = lv_arc_create(parent);
  lv_obj_set_pos(a, x, y);
  lv_obj_set_size(a, size, size);
  lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_arc_set_bg_angles(a, 0, 360);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(a, 0, 0);
  if (track) {
    lv_obj_set_style_arc_color(a, color, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
  } else {
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, color, LV_PART_INDICATOR);
  }
  return a;
}

static void spin(lv_obj_t* arc, uint32_t ms, bool reverse) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, arc);
  lv_anim_set_exec_cb(&a, spinCb);
  lv_anim_set_values(&a, reverse ? 360 : 0, reverse ? 0 : 360);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_linear);
  lv_anim_start(&a);
}

// Iron Man style arc reactor: static outer ring, two counter-rotating broken rings, glowing core.
static void reactor(lv_obj_t* parent, int x, int y, int size, bool animate) {
  arcRing(parent, x, y, size, max(1, size / 24), C_LINE, true);
  int m = size * 74 / 100, i = size * 48 / 100, c = max(4, size * 20 / 100);
  lv_obj_t* mid = arcRing(parent, x + (size - m) / 2, y + (size - m) / 2, m, max(2, size / 9), C_CYAN, false);
  lv_arc_set_angles(mid, 0, 270);
  lv_obj_t* in = arcRing(parent, x + (size - i) / 2, y + (size - i) / 2, i, max(2, size / 10), C_ICE, false);
  lv_arc_set_angles(in, 0, 120);
  rect(parent, x + (size - c) / 2, y + (size - c) / 2, c, c, C_CYAN, LV_RADIUS_CIRCLE);
  if (animate) {
    spin(mid, 3000, false);
    spin(in, 2000, true);
  }
}

struct Ring { lv_obj_t* seg[3]; lv_obj_t *pct, *sub; };

// Sprint ring: done (lime), review (violet), doing (cyan) as one segmented circle, % in the middle.
static void makeRing(Ring& r, lv_obj_t* parent, int x, int y, int size, int width) {
  lv_obj_t* track = arcRing(parent, x, y, size, width, C_DIM, true);
  r.seg[0] = arcRing(parent, x, y, size, width, C_LIME, false);
  r.seg[1] = arcRing(parent, x, y, size, width, C_VIOLET, false);
  r.seg[2] = arcRing(parent, x, y, size, width, C_CYAN, false);
  r.pct = hudText(track, F_HUD_L, C_LIME, "0%");
  lv_obj_align(r.pct, LV_ALIGN_CENTER, 0, -6);
  r.sub = hudText(track, F_HUD, C_MUTED, "");
  lv_obj_align(r.sub, LV_ALIGN_CENTER, 0, 14);
}

static void setRing(Ring& r, const SprintData& s) {
  float vals[3] = {0, 0, 0};
  for (int i = 0; i < s.count; i++) {
    const Card& c = s.cards[i];
    if (c.cat == CAT_DONE) vals[0] += c.pts;
    else if (c.cat == CAT_REVIEW) vals[1] += c.pts;
    else if (c.cat == CAT_PROGRESS) vals[2] += c.pts;
  }
  float acc = 0;
  for (int i = 0; i < 3; i++) {
    float deg = s.ptsTotal > 0 ? 360.0f * vals[i] / s.ptsTotal : 0;
    if (deg < 1) {
      lv_obj_add_flag(r.seg[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(r.seg[i], LV_OBJ_FLAG_HIDDEN);
      int start = 270 + (int)(360.0f * acc / s.ptsTotal);  // 270 = 12 o'clock
      if (deg >= 359) lv_arc_set_angles(r.seg[i], 0, 360);
      else lv_arc_set_angles(r.seg[i], start % 360, (start + (int)deg) % 360);
    }
    acc += vals[i];
  }
  int pct = s.ptsTotal > 0 ? (int)(100 * s.ptsDone / s.ptsTotal + 0.5f) : 0;
  lv_label_set_text_fmt(r.pct, "%d%%", pct);
  lv_label_set_text(r.sub, (fmtPts(s.ptsDone) + "/" + fmtPts(s.ptsTotal)).c_str());
}

static Ring nowRing, spRing;

// Ticket row: accent bar, key over title, outlined status pill, points. Built hidden; fillTicket shows it.
static TicketSlot makeTicket(lv_obj_t* parent, int x, int y, int w) {
  TicketSlot t;
  t.box = panel(parent, x, y, w, 42, 6);
  t.bar = rect(t.box, 8, 10, 3, 22, C_LINE);
  t.key = hudText(t.box, F_HUD_M, C_TEXT, "");
  lv_obj_set_pos(t.key, 18, 2);
  t.title = text(t.box, F_SMALL, C_TEXT, "");
  lv_obj_set_width(t.title, w - 170);
  lv_label_set_long_mode(t.title, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(t.title, 18, 19);
  t.pill = plain(t.box, w - 142, 11, 100, 20);
  lv_obj_set_style_radius(t.pill, 10, 0);
  lv_obj_set_style_border_width(t.pill, 1, 0);
  t.pillLbl = text(t.pill, F_HUD, C_TEXT, "");
  lv_obj_center(t.pillLbl);
  t.pts = hudText(t.box, F_HUD_M, C_MUTED, "");
  lv_obj_align(t.pts, LV_ALIGN_RIGHT_MID, -12, 0);
  lv_obj_add_flag(t.box, LV_OBJ_FLAG_HIDDEN);
  return t;
}

static void fillTicket(TicketSlot& t, const Card* c) {
  if (!c) { lv_obj_add_flag(t.box, LV_OBJ_FLAG_HIDDEN); return; }
  lv_color_t col = catColor(c->cat, c->blocked);
  lv_obj_set_style_bg_color(t.bar, col, 0);
  lv_label_set_text(t.key, c->key.c_str());
  lv_obj_set_style_text_color(t.key, col, 0);
  lv_label_set_text(t.title, c->title.c_str());
  lv_obj_set_style_text_color(t.title, c->cat == CAT_DONE ? C_MUTED : C_TEXT, 0);
  lv_obj_set_style_border_color(t.pill, col, 0);
  lv_label_set_text(t.pillLbl, catName(c->cat, c->blocked));
  lv_obj_set_style_text_color(t.pillLbl, col, 0);
  lv_label_set_text(t.pts, fmtPts(c->pts).c_str());
  lv_obj_remove_flag(t.box, LV_OBJ_FLAG_HIDDEN);
}

// One message label per list (errors, "no cards"); hidden when the list has rows.
static lv_obj_t* makeMsg(lv_obj_t* parent) {
  lv_obj_t* l = errorText(parent, 6, 6, "");
  lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
  return l;
}

static void setMsg(lv_obj_t* l, const char* s) {
  if (!s) { lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN); return; }
  lv_label_set_text(l, s);
  lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
}

// ---------- build: header, tab bar, banner (layer_top) ----------

static void blinkCb(void* o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t*)o, (lv_opa_t)v, 0); }

static void buildHeader() {
  header = panel(lv_layer_top(), 4, 2, 472, 34, 8);
  lv_obj_set_style_bg_color(header, C_BG, 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  reactor(header, 8, 5, 24, true);
  lv_obj_t* name = hudText(header, F_HUD_M, C_CYAN, "F.R.I.D.A.Y.");
  lv_obj_set_pos(name, 38, 8);  // ~120 px wide; the dot and status line start after it
  hdDot = rect(header, 164, 14, 6, 6, C_LIME, LV_RADIUS_CIRCLE);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, hdDot);
  lv_anim_set_exec_cb(&a, blinkCb);
  lv_anim_set_values(&a, 60, 255);
  lv_anim_set_duration(&a, 900);
  lv_anim_set_playback_duration(&a, 900);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
  hdPhrase = text(header, F_HUD, C_ICE, "");
  lv_obj_set_width(hdPhrase, 160);
  lv_label_set_long_mode(hdPhrase, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(hdPhrase, 178, 9);
  hdHealth = hudText(header, F_HUD, C_LIME, "");
  lv_obj_set_pos(hdHealth, 344, 9);
  // WiFi bars sit in their own gap between the health count and the clock (clock starts ~x 414).
  for (int i = 0; i < 4; i++) hdWifi[i] = rect(header, 390 + i * 5, 24 - (4 + i * 3), 3, 4 + i * 3, C_DIM);
  hdClock = hudText(header, F_HUD_M, C_TEXT, "--:--");
  lv_obj_align(hdClock, LV_ALIGN_RIGHT_MID, -10, 0);
  lv_obj_add_flag(header, LV_OBJ_FLAG_HIDDEN);
}

static void buildTabBar() {
  tabBar = panel(lv_layer_top(), 4, 268, 472, 48, 8);
  lv_obj_set_style_bg_color(tabBar, C_BG, 0);
  lv_obj_set_style_bg_opa(tabBar, LV_OPA_COVER, 0);
  for (int i = 0; i < PG_COUNT; i++) {
    if (i) {
      lv_point_precise_t sep[2] = {{(int32_t)(i * TAB_W), 10}, {(int32_t)(i * TAB_W), 38}};
      polyline(tabBar, sep, 2, C_DIM, 1);
    }
    tabLbl[i] = text(tabBar, F_HUD, C_MUTED, (String(PAGE_ICONS[i]) + " " + PAGE_TITLES[i]).c_str());
    lv_obj_set_width(tabLbl[i], TAB_W);
    lv_obj_set_style_text_align(tabLbl[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(tabLbl[i], LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(tabLbl[i], i * TAB_W, 15);
  }
  tabLine = rect(tabBar, 0, 40, 48, 2, C_CYAN);
  lv_obj_add_flag(tabBar, LV_OBJ_FLAG_HIDDEN);
}

static void buildBanner() {
  // Opaque (screen color) so it covers the tab bar; the chamfered outline carries the alert color.
  banner = plain(lv_layer_top(), 4, 266, 472, 50);
  lv_obj_set_style_bg_color(banner, C_BG, 0);
  lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
  int r = 471, b = 49, cut = 8;
  lv_point_precise_t p[9] = {{cut, 0}, {r - cut, 0}, {r, cut}, {r, b - cut}, {r - cut, b},
                             {cut, b}, {0, b - cut}, {0, cut}, {cut, 0}};
  bnLine = polyline(banner, p, 9, C_AMBER, 2);
  bnBar = rect(banner, 0, 9, 4, 32, C_AMBER);
  bnTag = hudText(banner, F_HUD, C_AMBER, "");
  lv_obj_set_pos(bnTag, 16, 5);
  bnText = text(banner, F_BODY, C_TEXT, "");
  lv_obj_set_width(bnText, 300);
  lv_label_set_long_mode(bnText, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(bnText, 16, 21);
  bnCount = plain(banner, 330, 16, 38, 18);
  lv_obj_set_style_border_width(bnCount, 1, 0);
  lv_obj_set_style_border_color(bnCount, C_RED, 0);
  bnCountLbl = hudText(bnCount, F_HUD, C_RED, "1/2");
  lv_obj_center(bnCountLbl);
  bnRight = hudText(banner, F_HUD_L, C_AMBER, "");
  lv_obj_align(bnRight, LV_ALIGN_RIGHT_MID, -12, 0);
  lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

// ---------- build: pages ----------

static const int TL_START = 9, TL_END = 18, TL_X = 10, TL_W = 206;  // day timeline 09:00-18:00

static int tlX(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  int min = (lt.tm_hour - TL_START) * 60 + lt.tm_min;
  return constrain(min * TL_W / ((TL_END - TL_START) * 60), 0, TL_W);
}

static void buildNow() {
  lv_obj_t* s = screens[PG_NOW] = newScreen();
  lv_obj_t* L = panel(s, 6, CONTENT_Y, 228, 130, 8, C_LINE, true);
  nowClock = text(L, F_CLOCK, C_CYAN, "--:--");
  lv_obj_set_style_text_letter_space(nowClock, 2, 0);
  lv_obj_set_pos(nowClock, 10, 2);
  nowJalali = hudText(L, F_HUD_M, C_TEXT, "");
  lv_obj_set_pos(nowJalali, 10, 56);
  nowDate = hudText(L, F_HUD, C_MUTED, "");
  lv_obj_set_pos(nowDate, 150, 60);
  for (int h = TL_START; h <= TL_END; h += 3) {
    lv_obj_t* l = hudText(L, F_HUD, C_MUTED, (h < 10 ? "0" + String(h) : String(h)).c_str());
    lv_obj_set_pos(l, TL_X + (h - TL_START) * TL_W / (TL_END - TL_START) - (h == TL_END ? 14 : 0), 78);
  }
  nowTlBar = rect(L, TL_X, 94, TL_W, 10, C_DIM);
  nowTlNow = rect(L, TL_X, 90, 2, 18, C_TEXT);
  nowMeet = hudText(L, F_HUD, C_AMBER, "");
  lv_obj_set_width(nowMeet, 210);
  lv_label_set_long_mode(nowMeet, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(nowMeet, 10, 110);

  lv_obj_t* R = panel(s, 240, CONTENT_Y, 234, 130, 8, C_LINE, true);
  nowSprint = hudText(R, F_HUD_M, C_TEXT, "");
  lv_obj_set_width(nowSprint, 120);
  lv_label_set_long_mode(nowSprint, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(nowSprint, 10, 6);
  lv_obj_set_pos(hudText(R, F_HUD, C_MUTED, "SPRINT PROGRESS"), 10, 26);
  lv_color_t legColors[4] = {C_LIME, C_VIOLET, C_CYAN, C_ICE};
  for (int i = 0; i < 4; i++) {
    rect(R, 10, 50 + i * 14, 6, 6, legColors[i]);
    nowLeg[i] = hudText(R, F_HUD, C_MUTED, "");
    lv_obj_set_pos(nowLeg[i], 20, 45 + i * 14);
  }
  nowDays = hudText(R, F_HUD, C_AMBER, "");
  lv_obj_set_pos(nowDays, 10, 108);
  makeRing(nowRing, R, 128, 16, 96, 9);

  nowRows = plain(s, 6, 176, 468, 90);
  for (int i = 0; i < 2; i++) nowTickets[i] = makeTicket(nowRows, 0, i * 46, 468);
  nowMsg = makeMsg(nowRows);
}

static void buildSprint() {
  lv_obj_t* s = screens[PG_SPRINT] = newScreen();
  lv_obj_t* L = panel(s, 6, CONTENT_Y, 200, 222, 8, C_LINE, true);
  makeRing(spRing, L, 25, 14, 150, 14);
  spFoot = hudText(L, F_HUD, C_MUTED, "");
  lv_obj_set_pos(spFoot, 12, 176);
  spDays = hudText(L, F_HUD_M, C_AMBER, "");
  lv_obj_set_pos(spDays, 12, 194);

  lv_obj_t* R = panel(s, 212, CONTENT_Y, 262, 222, 8, C_LINE, true);
  spName = hudText(R, F_HUD_M, C_TEXT, "");
  lv_obj_set_width(spName, 240);
  lv_label_set_long_mode(spName, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(spName, 10, 6);
  lv_obj_set_pos(hudText(R, F_HUD, C_MUTED, "STATUS"), 10, 28);
  for (int i = 0; i < 4; i++) {
    int y = 46 + i * 20;
    spBarLbl[i] = hudText(R, F_HUD, C_MUTED, "");
    lv_obj_set_pos(spBarLbl[i], 10, y);
    spBars[i] = lv_bar_create(R);
    lv_obj_set_size(spBars[i], 128, 6);
    lv_obj_set_pos(spBars[i], 120, y + 5);
    lv_obj_set_style_radius(spBars[i], 0, LV_PART_MAIN);
    lv_obj_set_style_radius(spBars[i], 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(spBars[i], C_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_color(spBars[i], catColor((CardCat)i, false), LV_PART_INDICATOR);
  }
  lv_obj_set_pos(hudText(R, F_HUD, C_MUTED, "BURNDOWN"), 10, 130);
  spChart = lv_chart_create(R);
  lv_obj_set_size(spChart, 240, 66);
  lv_obj_set_pos(spChart, 10, 148);
  lv_chart_set_type(spChart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(spChart, 0, 0);
  lv_obj_set_style_bg_opa(spChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spChart, 0, 0);
  lv_obj_set_style_radius(spChart, 0, 0);
  lv_obj_set_style_pad_all(spChart, 2, 0);
  lv_obj_set_style_line_width(spChart, 2, LV_PART_ITEMS);
  lv_obj_set_style_size(spChart, 0, 0, LV_PART_INDICATOR);
  spIdeal = lv_chart_add_series(spChart, C_LINE, LV_CHART_AXIS_PRIMARY_Y);
  spSeries = lv_chart_add_series(spChart, C_CYAN, LV_CHART_AXIS_PRIMARY_Y);
  spBurnNote = hudText(R, F_HUD, C_MUTED, "");
  lv_obj_set_pos(spBurnNote, 10, 170);
}

static void buildAgenda() {
  lv_obj_t* s = screens[PG_AGENDA] = newScreen();
  agList = plain(s, 6, CONTENT_Y, 468, 224);
  agMsg = makeMsg(agList);
  agLayout = -1;  // agenda slots are sized on the first render
  for (MeetSlot& m : agRows) m.box = nullptr;
}

static void buildCards() {
  lv_obj_t* s = screens[PG_CARDS] = newScreen();
  cardList = plain(s, 6, CONTENT_Y, 468, 224);
  cardRowsBuilt = 0;  // rows are created by drawCardPage, only as many as a page shows
  cardMsg = makeMsg(cardList);
}

static void buildKpi() {
  lv_obj_t* s = screens[PG_KPI] = newScreen();
  for (int i = 0; i < 6; i++) {
    int col = i % 3, row = i / 3;
    lv_obj_t* t = panel(s, 6 + col * 158, CONTENT_Y + row * 112, 152, 106, 8, C_LINE, i == 0 || i == 5);
    kpiLbl[i] = hudText(t, F_HUD, C_MUTED, "");
    lv_obj_set_width(kpiLbl[i], 132);
    lv_label_set_long_mode(kpiLbl[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(kpiLbl[i], 10, 6);
    kpiVal[i] = text(t, F_CLOCK, C_TEXT, "-");
    lv_obj_set_pos(kpiVal[i], 10, 24);
    kpiSub[i] = hudText(t, F_HUD, C_MUTED, "");
    lv_obj_set_width(kpiSub[i], 132);
    lv_label_set_long_mode(kpiSub[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(kpiSub[i], 10, 84);
  }
}

static void buildHealth() {
  lv_obj_t* s = screens[PG_HEALTH] = newScreen();
  lv_obj_set_pos(hudText(s, F_HUD, C_MUTED, "UPTIME KUMA // SERVICES"), 12, CONTENT_Y + 2);
  hlSummary = hudText(s, F_HUD, C_MUTED, "");
  lv_obj_align(hlSummary, LV_ALIGN_TOP_RIGHT, -12, CONTENT_Y + 2);
  hlGrid = plain(s, 6, CONTENT_Y + 20, 468, 204);
  hlTilesBuilt = 0;  // tiles are created by renderHealth as monitors appear
  hlMsg = makeMsg(hlGrid);
}

// ---------- refresh screen ----------

static lv_obj_t* loadScreen;
static lv_obj_t *loadIcon[STEP_COUNT], *loadDetail[STEP_COUNT], *loadTitle;
static bool dashboardBuilt = false;

static void buildLoading() {
  static const char* names[STEP_COUNT] = {"JIRA CARDS", "SPRINT DATES", "KPI QUERIES", "UPTIME KUMA", "CALENDAR"};
  loadScreen = newScreen();
  reactor(loadScreen, 12, 8, 46, false);
  lv_obj_t* t = hudText(loadScreen, F_HUD_L, C_CYAN, "F.R.I.D.A.Y.");
  lv_obj_set_style_text_letter_space(t, 3, 0);
  lv_obj_set_pos(t, 70, 10);
  loadTitle = hudText(loadScreen, F_HUD, C_MUTED, "SYNCING DATA");
  lv_obj_set_pos(loadTitle, 72, 38);
  for (int i = 0; i < STEP_COUNT; i++) {
    lv_obj_t* row = panel(loadScreen, 6, 64 + i * 50, 468, 44, 6);
    loadIcon[i] = text(row, F_HUD_M, C_MUTED, "");
    lv_obj_set_pos(loadIcon[i], 12, 12);
    lv_obj_set_pos(hudText(row, F_HUD_M, C_TEXT, names[i]), 40, 12);
    loadDetail[i] = hudText(row, F_HUD, C_MUTED, "");
    lv_obj_set_width(loadDetail[i], 240);
    lv_label_set_long_mode(loadDetail[i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(loadDetail[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(loadDetail[i], 216, 15);
  }
}

static void onProgress(FetchStep step, StepState state, const String& detail) {
  static const char* icons[] = {"", LV_SYMBOL_REFRESH, LV_SYMBOL_OK, LV_SYMBOL_CLOSE, LV_SYMBOL_MINUS};
  lv_color_t colors[] = {C_MUTED, C_CYAN, C_LIME, C_RED, C_MUTED};
  lv_label_set_text(loadIcon[step], icons[state]);
  lv_obj_set_style_text_color(loadIcon[step], colors[state], 0);
  String d = state == STEP_RUN ? String("LOADING...") : detail;
  lv_label_set_text(loadDetail[step], d.c_str());
  lv_obj_set_style_text_color(loadDetail[step], state == STEP_FAIL ? C_RED : C_ICE, 0);
  lv_refr_now(nullptr);  // draw now: the next fetch blocks the loop
}

void ui_init() {
  bootScreen = newScreen();
  reactor(bootScreen, (SCREEN_W - 110) / 2, 26, 110, true);
  lv_obj_t* name = hudText(bootScreen, F_HUD_L, C_CYAN, "F.R.I.D.A.Y.");
  lv_obj_set_style_text_letter_space(name, 4, 0);
  lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 150);
  bootTitle = hudText(bootScreen, F_HUD_M, C_TEXT, "");
  lv_obj_align(bootTitle, LV_ALIGN_TOP_MID, 0, 186);
  bootDetail = hudText(bootScreen, F_HUD, C_ICE, "");
  lv_obj_set_width(bootDetail, 440);
  lv_obj_set_style_text_align(bootDetail, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(bootDetail, LV_LABEL_LONG_WRAP);
  lv_obj_align(bootDetail, LV_ALIGN_TOP_MID, 0, 214);
  lv_screen_load(bootScreen);

  buildLoading();
  buildHeader();
  buildTabBar();
  buildBanner();
}

static void buildDashboard() {
  buildNow();
  buildAgenda();
  buildSprint();
  buildCards();
  buildKpi();
  buildHealth();
  dashboardBuilt = true;
  bannerMode = -1;
  cardsDwellMs = 0;
}

void ui_boot(const String& title, const String& detail) {
  String t = title;
  t.toUpperCase();
  lv_label_set_text(bootTitle, t.c_str());
  lv_obj_align(bootTitle, LV_ALIGN_TOP_MID, 0, 186);
  lv_label_set_text(bootDetail, detail.c_str());
  lv_timer_handler();
}

// ---------- data → widgets ----------

static const Card* currentCard() {
  const SprintData& s = snap.sprint;
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < s.count; i++)
      if (s.cards[i].cat == (pass == 0 ? CAT_PROGRESS : CAT_REVIEW)) return &s.cards[i];
  return nullptr;
}

// Second row on the home screen: what needs attention next (blocked, then review, then to do).
static const Card* urgentCard(const Card* skip) {
  const SprintData& s = snap.sprint;
  for (int pass = 0; pass < 3; pass++)
    for (int i = 0; i < s.count; i++) {
      const Card& c = s.cards[i];
      if (&c == skip) continue;
      if ((pass == 0 && c.blocked) || (pass == 1 && c.cat == CAT_REVIEW) || (pass == 2 && c.cat == CAT_TODO)) return &c;
    }
  return nullptr;
}

static const Meeting* nextMeeting(time_t now) {
  for (int i = 0; i < snap.meet.count; i++)
    if (snap.meet.m[i].end > now) return &snap.meet.m[i];
  return nullptr;
}

// Minute-level parts of the home screen: next meeting line and the "now" marker.
static void renderNowTime(time_t now) {
  const Meeting* m = nextMeeting(now);
  if (m) {
    long mins = (m->start - now) / 60;
    String when = m->start > now ? (mins >= 60 ? "IN " + String(mins / 60) + "H " + String(mins % 60) + "M" : "IN " + String(mins) + "M") : String("NOW");
    String line = m->title + "  " + hhmm(m->start) + " · " + when + (m->where.length() ? " · " + m->where : "");
    lv_label_set_text(nowMeet, line.c_str());
    lv_obj_set_style_text_color(nowMeet, m->start - now <= MEETING_SOON_SEC ? C_AMBER : C_ICE, 0);
  } else if (snap.meet.configured && !snap.meet.ok) {
    lv_label_set_text(nowMeet, ("CALENDAR: " + snap.meet.err).c_str());
    lv_obj_set_style_text_color(nowMeet, C_RED, 0);
  } else {
    lv_label_set_text(nowMeet, snap.meet.configured ? "NO MORE MEETINGS TODAY" : "CALENDAR NOT SET");
    lv_obj_set_style_text_color(nowMeet, C_MUTED, 0);
  }
  lv_obj_set_x(nowTlNow, TL_X + tlX(now) - 1);
}

static void renderNow(time_t now) {
  // Timeline blocks: one per meeting today; the next one is amber.
  lv_obj_clean(nowTlBar);
  const Meeting* next = nextMeeting(now);
  for (int i = 0; i < snap.meet.count; i++) {
    const Meeting& m = snap.meet.m[i];
    int x0 = tlX(m.start), x1 = max(x0 + 3, tlX(m.end));
    rect(nowTlBar, x0, 0, x1 - x0, 10, &m == next ? C_AMBER : C_LINE);
  }
  renderNowTime(now);

  const SprintData& s = snap.sprint;
  lv_label_set_text(nowSprint, sprintShort(s.name).c_str());
  int counts[4] = {}, open = 0;
  for (int i = 0; i < s.count; i++) {
    counts[s.cards[i].cat]++;
    if (s.cards[i].cat != CAT_DONE) open++;
  }
  lv_label_set_text_fmt(nowLeg[0], "DONE %d", counts[CAT_DONE]);
  lv_label_set_text_fmt(nowLeg[1], "REVIEW %d", counts[CAT_REVIEW]);
  lv_label_set_text_fmt(nowLeg[2], "DOING %d", counts[CAT_PROGRESS]);
  lv_label_set_text_fmt(nowLeg[3], "TODO %d", counts[CAT_TODO]);
  if (s.endTs) {
    long days = (s.endTs - now + 86399) / 86400;
    lv_label_set_text(nowDays, days > 0 ? (String(days) + (days == 1 ? " DAY LEFT" : " DAYS LEFT")).c_str() : "SPRINT ENDED");
  } else {
    lv_label_set_text(nowDays, snap.mock ? "DEMO DATA" : "");
  }
  setRing(nowRing, s);

  const Card* c = currentCard();
  const Card* u = urgentCard(c);
  if (!s.ok || (!c && !u)) {
    fillTicket(nowTickets[0], nullptr);
    fillTicket(nowTickets[1], nullptr);
    setMsg(nowMsg, !s.ok ? ("JIRA: " + s.err).c_str() : "NO ACTIVE CARDS · PICK ONE FROM THE SPRINT");
  } else {
    setMsg(nowMsg, nullptr);
    fillTicket(nowTickets[0], c ? c : u);
    fillTicket(nowTickets[1], c ? u : nullptr);
  }
}

// Agenda slots for a list of n rows: taller rows when there are 3 or fewer, so long links and
// locations wrap onto a second line. Rebuilt only when n changes (a meeting ends, new data).
static void layoutAgenda(int n) {
  if (n == agLayout) return;
  for (MeetSlot& m : agRows)
    if (m.box) { lv_obj_delete(m.box); m.box = nullptr; }
  agLayout = n;
  int step = min(74, 224 / max(1, n)), h = step - 4;
  bool twoLines = step >= 60;
  for (int i = 0; i < n; i++) {
    MeetSlot& m = agRows[i];
    m.box = panel(agList, 0, i * step, 468, h, 6, C_LINE, false, &m.outline);
    m.bar = rect(m.box, 8, 8, 3, h - 16, C_ICE);
    m.start = hudText(m.box, F_HUD_M, C_TEXT, "");
    lv_obj_set_pos(m.start, 18, 2);
    m.end = hudText(m.box, F_HUD, C_MUTED, "");
    lv_obj_set_pos(m.end, 18, 21);
    m.title = text(m.box, F_SMALL, C_TEXT, "");
    lv_obj_set_width(m.title, 280);
    lv_label_set_long_mode(m.title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(m.title, 80, 1);
    m.where = hudText(m.box, F_HUD, C_CYAN, "");
    lv_obj_set_width(m.where, 280);
    if (twoLines) {
      lv_label_set_long_mode(m.where, LV_LABEL_LONG_WRAP);  // links wrap onto a second line
      lv_obj_set_height(m.where, 36);
    } else {
      lv_label_set_long_mode(m.where, LV_LABEL_LONG_DOT);
    }
    lv_obj_set_pos(m.where, 80, 22);
    m.pill = plain(m.box, 468 - 96, (h - 20) / 2 - 2, 84, 20);
    lv_obj_set_style_radius(m.pill, 10, 0);
    lv_obj_set_style_border_width(m.pill, 1, 0);
    m.pillLbl = text(m.pill, F_HUD, C_ICE, "");
    lv_obj_center(m.pillLbl);
  }
}

// Agenda: the meetings that haven't ended yet (up to 4): time, title, room or link, state.
static void renderAgenda(time_t now) {
  if (!agList) return;
  const MeetingData& d = snap.meet;
  int first = 0;
  while (first < d.count && d.m[first].end <= now) first++;
  int n = d.configured && d.ok ? min(d.count - first, MAX_AGENDA_ROWS) : 0;
  layoutAgenda(n);
  if (!n) {
    String err = "CALENDAR: " + d.err;
    setMsg(agMsg, !d.configured ? "CALENDAR NOT SET · ADD AN .ICS URL ON THE SETTINGS PAGE"
                  : !d.ok       ? err.c_str()
                  : d.count     ? "NO MORE MEETINGS TODAY"
                                : "NO MEETINGS TODAY");
    return;
  }
  setMsg(agMsg, nullptr);
  const Meeting* next = nextMeeting(now);
  for (int row = 0; row < n; row++) {
    const Meeting& m = d.m[first + row];
    MeetSlot& r = agRows[row];
    bool live = m.start <= now;
    lv_color_t col = live ? C_RED : &m == next ? C_AMBER : C_ICE;
    lv_obj_set_style_line_color(r.outline, live ? C_RED : C_LINE, 0);
    lv_obj_set_style_bg_color(r.bar, col, 0);
    lv_label_set_text(r.start, hhmm(m.start).c_str());
    lv_label_set_text(r.end, hhmm(m.end).c_str());
    lv_label_set_text(r.title, m.title.c_str());
    lv_label_set_text(r.where, m.where.length() ? m.where.c_str() : "-");
    long mins = (m.start - now) / 60;
    String state = live ? String("LIVE") : mins >= 60 ? "IN " + String(mins / 60) + "H" : "IN " + String(max(1L, mins)) + "M";
    lv_obj_set_style_border_color(r.pill, col, 0);
    lv_label_set_text(r.pillLbl, state.c_str());
    lv_obj_set_style_text_color(r.pillLbl, col, 0);
  }
}

static void renderSprint(time_t now) {
  const SprintData& s = snap.sprint;
  setRing(spRing, s);
  lv_label_set_text(spName, sprintShort(s.name).c_str());
  if (s.endTs) {
    long days = (s.endTs - now + 86399) / 86400;
    lv_label_set_text(spDays, days > 0 ? (String(days) + (days == 1 ? " DAY LEFT" : " DAYS LEFT")).c_str() : "SPRINT ENDED");
    lv_obj_set_style_text_color(spDays, days <= 2 ? C_RED : C_AMBER, 0);
  } else {
    lv_label_set_text(spDays, "NO END DATE");
  }

  int counts[4] = {}, blocked = 0;
  for (int i = 0; i < s.count; i++) {
    counts[s.cards[i].cat]++;
    if (s.cards[i].blocked) blocked++;
  }
  for (int i = 0; i < 4; i++) {
    lv_label_set_text_fmt(spBarLbl[i], "%s %d", catName((CardCat)i, false), counts[i]);
    lv_bar_set_range(spBars[i], 0, max(1, s.count));
    lv_bar_set_value(spBars[i], counts[i], LV_ANIM_OFF);
  }
  String foot = String(s.count) + " CARDS";
  if (blocked) foot += " · " + String(blocked) + " BLOCKED";
  lv_label_set_text(spFoot, foot.c_str());

  // Burndown from Jira itself: points remaining at the end of each sprint day,
  // from when each done card was resolved. Dim line = ideal.
  if (!s.startTs || !s.endTs || s.endTs <= s.startTs) {
    lv_obj_add_flag(spChart, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(spBurnNote, "SET THE BOARD ID TO SEE IT");
    return;
  }
  lv_obj_remove_flag(spChart, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(spBurnNote, "");
  struct tm st;
  localtime_r(&s.startTs, &st);
  st.tm_hour = st.tm_min = st.tm_sec = 0;
  time_t day0 = mktime(&st);
  int days = constrain((int)((s.endTs - day0 + 86399) / 86400), 2, 31);
  lv_chart_set_point_count(spChart, days);
  lv_chart_set_range(spChart, LV_CHART_AXIS_PRIMARY_Y, 0, max(1, (int)(s.ptsTotal + 0.5f)));
  for (int d = 0; d < days; d++) {
    time_t dayEnd = day0 + (d + 1) * 86400L;
    lv_chart_set_value_by_id(spChart, spIdeal, d, (int32_t)(s.ptsTotal * (days - 1 - d) / (days - 1) + 0.5f));
    if (dayEnd - 86400 > now) {
      lv_chart_set_value_by_id(spChart, spSeries, d, LV_CHART_POINT_NONE);
      continue;
    }
    float left = s.ptsTotal;
    for (int i = 0; i < s.count; i++)
      if (s.cards[i].cat == CAT_DONE && s.cards[i].resolved && s.cards[i].resolved < dayEnd) left -= s.cards[i].pts;
    lv_chart_set_value_by_id(spChart, spSeries, d, (int32_t)(left + 0.5f));
  }
  lv_chart_refresh(spChart);
}

// The card list is shown in pages of CARDS_PER_PAGE, flipped every CARD_PAGE_MS like a departures board.
// (A continuous scroll redraws the whole list every frame; at ~10 fps on the ILI9488 it was unreadable.)
// Page size, timing and "hide Done" come from the settings page.
#define CARDS_PER_PAGE ((int)gCfg.cardsPerPage)
#define CARD_PAGE_MS (gCfg.cardPageSec * 1000UL)
static int cardOrder[MAX_CARDS];
static int cardOrderLen = 0, cardPage = 0;
static lv_timer_t* cardTimer = nullptr;

static int cardPages() { return max(1, (cardOrderLen + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE); }

static void cardTitle() {
  String t = cardPages() > 1 ? String(PAGE_TITLES[PG_CARDS]) + " " + String(cardPage + 1) + "/" + String(cardPages())
                             : String(PAGE_ICONS[PG_CARDS]) + " " + PAGE_TITLES[PG_CARDS];
  lv_label_set_text(tabLbl[PG_CARDS], t.c_str());
}

static void drawCardPage() {
  const SprintData& s = snap.sprint;
  bool empty = !s.count || !cardOrderLen;
  String err = "JIRA: " + s.err;
  setMsg(cardMsg, !empty ? nullptr : !s.ok ? err.c_str() : s.count ? "ALL CARDS ARE DONE" : "NO CARDS IN THIS SPRINT");
  int first = cardPage * CARDS_PER_PAGE;
  int need = empty ? 0 : min(min(CARDS_PER_PAGE, MAX_CARD_ROWS), cardOrderLen);
  for (; cardRowsBuilt < need; cardRowsBuilt++)
    cardRows[cardRowsBuilt] = makeTicket(cardList, 0, cardRowsBuilt * 45, 468);
  for (int i = 0; i < cardRowsBuilt; i++) {
    int n = first + i;
    bool show = !empty && i < CARDS_PER_PAGE && n < cardOrderLen;
    fillTicket(cardRows[i], show ? &s.cards[cardOrder[n]] : nullptr);
  }
  cardTitle();
}

static void cardTimerCb(lv_timer_t*) {
  // Not during a refresh: the dashboard (and cardList) is deleted then.
  if (!started || page != PG_CARDS || cardPages() < 2) return;
  cardPage = (cardPage + 1) % cardPages();
  drawCardPage();
}

// Restart at the first page (when the cards slide is shown).
static void startCardPages() {
  cardPage = 0;
  drawCardPage();
  if (!cardTimer) cardTimer = lv_timer_create(cardTimerCb, CARD_PAGE_MS, nullptr);
  lv_timer_set_period(cardTimer, CARD_PAGE_MS);
  lv_timer_reset(cardTimer);
  cardsDwellMs = cardPages() * CARD_PAGE_MS;
}

static void renderCards() {
  const SprintData& s = snap.sprint;
  // Active work first, done last.
  static const CardCat order[] = {CAT_PROGRESS, CAT_REVIEW, CAT_TODO, CAT_DONE};
  cardOrderLen = 0;
  for (CardCat cat : order) {
    if (cat == CAT_DONE && gCfg.hideDone) continue;
    for (int i = 0; i < s.count; i++)
      if (s.cards[i].cat == cat) cardOrder[cardOrderLen++] = i;
  }
  cardsDwellMs = cardPages() * CARD_PAGE_MS;
  if (cardPage >= cardPages()) cardPage = 0;
  drawCardPage();
}

static void setKpi(int i, const String& label, const String& value, const String& sub, lv_color_t color = C_TEXT) {
  lv_label_set_text(kpiLbl[i], label.c_str());
  lv_label_set_text(kpiVal[i], value.c_str());
  lv_label_set_text(kpiSub[i], sub.c_str());
  lv_obj_set_style_text_color(kpiVal[i], color, 0);
}

static void renderKpi() {
  const SprintData& s = snap.sprint;
  int inProg = 0, review = 0, blocked = 0, open = 0;
  for (int i = 0; i < s.count; i++) {
    const Card& c = s.cards[i];
    if (c.cat == CAT_PROGRESS) inProg++;
    if (c.cat == CAT_REVIEW) review++;
    if (c.blocked) blocked++;
    if (c.cat != CAT_DONE) open++;
  }
  int pct = s.ptsTotal > 0 ? (int)(100 * s.ptsDone / s.ptsTotal + 0.5f) : 0;
  setKpi(0, "POINTS DONE", fmtPts(s.ptsDone), "OF " + fmtPts(s.ptsTotal) + " PTS", C_LIME);
  setKpi(1, "SPRINT SCOPE", String(pct) + "%", String(open) + " CARDS OPEN", C_CYAN);
  setKpi(2, "WIP", String(inProg + review), String(inProg) + " DOING · " + String(review) + " REVIEW", inProg + review > 3 ? C_AMBER : C_TEXT);
  setKpi(3, "BLOCKED", String(blocked), blocked ? "NEEDS ATTENTION" : "ALL CLEAR", blocked ? C_AMBER : C_LIME);
  String k1 = gCfg.kpi1Label, k2 = gCfg.kpi2Label;
  k1.toUpperCase();
  k2.toUpperCase();
  setKpi(4, k1, s.kpi1 < 0 ? "-" : String(s.kpi1), "JIRA QUERY", s.kpi1 > 0 ? C_AMBER : C_TEXT);
  setKpi(5, k2, s.kpi2 < 0 ? "-" : String(s.kpi2), "JIRA QUERY", s.kpi2 > 0 ? C_RED : C_LIME);
}

// Creates tiles up to n (they are kept and reused; only as many as there are monitors).
static void ensureHealthTiles(int n) {
  for (; hlTilesBuilt < n; hlTilesBuilt++) {
    int i = hlTilesBuilt;
    HealthSlot& t = hlTiles[i];
    t.box = panel(hlGrid, (i % 2) * 237, (i / 2) * 52, 231, 46, 6, C_LINE, false, &t.outline);
    t.dot = rect(t.box, 10, 10, 8, 8, C_LIME, LV_RADIUS_CIRCLE);
    t.name = hudText(t.box, F_HUD_M, C_TEXT, "");
    lv_obj_set_width(t.name, 196);
    lv_label_set_long_mode(t.name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t.name, 26, 4);
    t.sub = hudText(t.box, F_HUD, C_MUTED, "");
    lv_obj_set_pos(t.sub, 26, 26);
    t.track = rect(t.box, 150, 32, 70, 3, C_DIM);
    t.fill = rect(t.box, 150, 32, 1, 3, C_LIME);
  }
}

static void renderHealth() {
  const HealthData& h = snap.health;
  if (!h.ok && !h.count) {
    for (int i = 0; i < hlTilesBuilt; i++) lv_obj_add_flag(hlTiles[i].box, LV_OBJ_FLAG_HIDDEN);
    String err = "KUMA: " + h.err;
    setMsg(hlMsg, h.err.length() ? err.c_str() : "LOADING...");
    lv_label_set_text(hlSummary, "");
    return;
  }
  setMsg(hlMsg, nullptr);
  float upSum = 0;
  int upN = 0;
  ensureHealthTiles(min(h.count, MAX_HEALTH_TILES));
  for (int i = 0; i < hlTilesBuilt; i++) {
    HealthSlot& t = hlTiles[i];
    if (i >= h.count) { lv_obj_add_flag(t.box, LV_OBJ_FLAG_HIDDEN); continue; }
    const Monitor& m = h.m[i];
    lv_obj_set_style_line_color(t.outline, m.up ? C_LINE : C_RED, 0);
    lv_obj_set_style_bg_color(t.dot, m.up ? C_LIME : C_RED, 0);
    // Long monitor names (e.g. "CRM-CheckedCache-LoginTest") use the small font so they fit the tile.
    lv_obj_set_style_text_font(t.name, m.name.length() > 18 ? F_HUD : F_HUD_M, 0);
    lv_obj_set_style_text_color(t.name, m.up ? C_TEXT : C_RED, 0);
    lv_label_set_text(t.name, m.name.c_str());
    String sub = m.up ? (m.ping >= 0 ? String(m.ping) + " MS" : String("UP")) : String("DOWN");
    if (m.uptime >= 0) sub += " · " + String(m.uptime * 100, 1) + "%";
    lv_label_set_text(t.sub, sub.c_str());
    lv_obj_set_style_text_color(t.sub, m.up ? C_MUTED : C_RED, 0);
    if (m.uptime >= 0) {
      lv_obj_remove_flag(t.track, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(t.fill, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_width(t.fill, max(1, (int)(70 * m.uptime)));
      lv_obj_set_style_bg_color(t.fill, m.uptime > 0.99f ? C_LIME : m.uptime > 0.9f ? C_AMBER : C_RED, 0);
      upSum += m.uptime;
      upN++;
    } else {
      lv_obj_add_flag(t.track, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(t.fill, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(t.box, LV_OBJ_FLAG_HIDDEN);
  }
  String sum = String(h.up) + "/" + String(h.count) + " UP";
  if (upN) sum += " · 24H " + String(100 * upSum / upN, 2) + "%";
  lv_label_set_text(hlSummary, sum.c_str());
  lv_obj_set_style_text_color(hlSummary, h.up == h.count ? C_LIME : C_RED, 0);
}

static void renderAll(time_t now) {
  LOG("render: %d cards, %d monitors, %d meetings", snap.sprint.count, snap.health.count, snap.meet.count);
  renderNow(now);
  renderAgenda(now);
  renderSprint(now);
  renderCards();
  renderKpi();
  renderHealth();
  const HealthData& h = snap.health;
  if (h.count) {
    lv_label_set_text_fmt(hdHealth, "%s %d/%d", h.up == h.count ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, h.up, h.count);
    lv_obj_set_style_text_color(hdHealth, h.up == h.count ? C_LIME : C_RED, 0);
  } else {
    lv_label_set_text(hdHealth, "");
  }
}

// ---------- alerts: meeting soon / meeting live / outage ----------
// Same rules as before; when several are active the banner rotates through them every 4 s
// and shows a "1/2" counter.

enum { BN_NONE, BN_SOON, BN_STARTED, BN_DOWN };
struct Alert { int mode; String tag, text, right; };
static const int MAX_ALERTS = 6;
static Alert alerts[MAX_ALERTS];
static int alertCount = 0, alertIdx = 0;
static uint32_t alertShownAt = 0;

static void bnPulseCb(void*, int32_t v) {
  lv_obj_set_style_bg_opa(bnBar, (lv_opa_t)v, 0);
  lv_obj_set_style_line_opa(bnLine, (lv_opa_t)v, 0);
}

static void styleBanner(int mode) {
  if (mode == bannerMode) return;
  bannerMode = mode;
  lv_anim_delete(banner, bnPulseCb);
  if (mode == BN_NONE) {
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(banner, LV_OBJ_FLAG_HIDDEN);
  lv_color_t c = mode == BN_SOON ? C_AMBER : C_RED;
  lv_obj_set_style_line_color(bnLine, c, 0);
  lv_obj_set_style_line_opa(bnLine, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bnBar, c, 0);
  lv_obj_set_style_bg_opa(bnBar, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(bnTag, c, 0);
  lv_obj_set_style_text_color(bnRight, c, 0);
  lv_obj_set_style_border_color(bnCount, c, 0);
  lv_obj_set_style_text_color(bnCountLbl, c, 0);
  if (mode != BN_SOON) {
    // Pulse only the outline and the accent bar: a few hundred pixels, cheap on the ILI9488.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, banner);
    lv_anim_set_values(&a, 90, 255);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_playback_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, bnPulseCb);
    lv_anim_start(&a);
  }
}

static void addAlert(int mode, const String& tag, const String& text, const String& right) {
  if (alertCount < MAX_ALERTS) alerts[alertCount++] = {mode, tag, text, right};
}

static void collectAlerts(time_t now) {
  alertCount = 0;
  for (int i = 0; i < snap.meet.count; i++) {
    const Meeting& m = snap.meet.m[i];
    long since = now - m.start;
    if (since >= 0 && since < MEETING_ALERT_SEC)
      addAlert(BN_STARTED, LV_SYMBOL_PLAY " MEETING LIVE", m.title + (m.where.length() ? " · " + m.where : ""), "+" + mmss(since));
  }
  const HealthData& h = snap.health;
  if (h.ok && h.up < h.count) {
    String names;
    for (int i = 0; i < h.count; i++)
      if (!h.m[i].up) names += (names.length() ? ", " : "") + h.m[i].name;
    addAlert(BN_DOWN, LV_SYMBOL_WARNING " SERVICE DOWN", names, String(h.count - h.up) + "/" + String(h.count));
  }
  for (int i = 0; i < snap.meet.count; i++) {
    const Meeting& m = snap.meet.m[i];
    long until = m.start - now;
    if (until > 0 && until <= MEETING_SOON_SEC)
      addAlert(BN_SOON, LV_SYMBOL_BELL " NEXT MEETING · " + hhmm(m.start), m.title + (m.where.length() ? " · " + m.where : ""), mmss(until));
  }
}

static void updateBanner(time_t now) {
  collectAlerts(now);
  if (!alertCount) {
    styleBanner(BN_NONE);
    alertIdx = 0;
    return;
  }
  if (alertIdx >= alertCount) alertIdx = 0;
  if (alertCount > 1 && millis() - alertShownAt >= 4000) {
    alertIdx = (alertIdx + 1) % alertCount;
    alertShownAt = millis();
  }
  const Alert& a = alerts[alertIdx];
  styleBanner(a.mode);
  lv_label_set_text(bnTag, a.tag.c_str());
  lv_label_set_text(bnText, a.text.c_str());
  lv_label_set_text(bnRight, a.right.c_str());
  lv_obj_align(bnRight, LV_ALIGN_RIGHT_MID, -12, 0);
  if (alertCount > 1) {
    lv_obj_remove_flag(bnCount, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(bnCountLbl, "%d/%d", alertIdx + 1, alertCount);
  } else {
    lv_obj_add_flag(bnCount, LV_OBJ_FLAG_HIDDEN);
  }
}

// ---------- header: F.R.I.D.A.Y. status line, health, WiFi, clock ----------

static String fridayPhrase(const struct tm& lt) {
  if (alertCount) {
    const Alert& a = alerts[0];
    if (a.mode == BN_DOWN) return a.right.substring(0, a.right.indexOf('/')) + " SERVICE(S) DOWN, " + gCfg.ownerName;
    if (a.mode == BN_STARTED) return "YOU'RE LIVE, " + gCfg.ownerName;
    return "MEETING IN " + a.right.substring(0, a.right.indexOf(':')) + " MIN";
  }
  const SprintData& s = snap.sprint;
  switch ((millis() / 8000) % 4) {
    case 0: {
      int h = lt.tm_hour;
      const char* g = h < 5 ? "LATE SHIFT, " : h < 12 ? "GOOD MORNING, " : h < 17 ? "GOOD AFTERNOON, " : h < 22 ? "GOOD EVENING, " : "LATE SHIFT, ";
      return String(g) + gCfg.ownerName;
    }
    case 1:
      if (snap.health.count) return snap.health.up == snap.health.count ? "ALL SYSTEMS NOMINAL" : "SYSTEMS DEGRADED";
      return "HEALTH FEED OFFLINE";
    case 2: {
      if (!s.ok) return "JIRA LINK DOWN";
      int open = 0;
      for (int i = 0; i < s.count; i++) if (s.cards[i].cat != CAT_DONE) open++;
      return String(open) + " CARDS IN FLIGHT";
    }
    default:
      if (snap.mock) return "DEMO DATA";
      return s.updated ? "SYNCED " + hhmm(s.updated) : "AWAITING SYNC";
  }
}

static void updateHeader(time_t now, const struct tm& lt) {
  char clock[8];
  strftime(clock, sizeof(clock), "%H:%M", &lt);
  lv_label_set_text(hdClock, clock);
  String p = fridayPhrase(lt);
  p.toUpperCase();
  if (p != hdPhraseText) {
    hdPhraseText = p;
    lv_label_set_text(hdPhrase, p.c_str());
    lv_obj_set_style_text_color(hdPhrase, alertCount ? (alerts[0].mode == BN_SOON ? C_AMBER : C_RED) : C_ICE, 0);
  }
  bool wifi = WiFi.status() == WL_CONNECTED;
  int rssi = wifi ? WiFi.RSSI() : -100;
  int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -85 ? 1 : 0;
  for (int i = 0; i < 4; i++) lv_obj_set_style_bg_color(hdWifi[i], !wifi ? C_RED : i < bars ? C_CYAN : C_DIM, 0);
  lv_obj_set_style_bg_color(hdDot, wifi ? C_LIME : C_RED, 0);
}

// ---------- loop ----------

// The ILI9488 over SPI does ~10 full frames/s, so the slide is short: a quick swipe, not a slow judder.
static void showPage(int p, bool animate) {
  LOG("page %s", PAGE_TITLES[p]);
  page = p;
  lv_screen_load_anim(screens[p], animate ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_NONE, animate ? 250 : 0, 0, false);
  for (int i = 0; i < PG_COUNT; i++) lv_obj_set_style_text_color(tabLbl[i], i == p ? C_CYAN : C_MUTED, 0);
  lv_obj_set_x(tabLine, p * TAB_W + (TAB_W - 48) / 2);
  if (p == PG_CARDS) startCardPages();
  else cardTitle();
  lastSwitch = millis();
}

// The cards page stays until every page of cards has been shown once.
static uint32_t pageDwellMs(int p) {
  uint32_t base = gCfg.rotateSec * 1000UL;
  return p == PG_CARDS ? max(base, cardsDwellMs) : base;
}

void ui_apply_settings() {
#ifdef TFT_BL
  analogWrite(TFT_BL, gCfg.brightness);
#endif
}

void ui_tick() {
  if (!started) return;
  time_t now = time(nullptr);

  model_lock();
  bool changed = gData.version != seenVersion;
  if (changed) {
    snap = gData;
    seenVersion = gData.version;
  }
  model_unlock();
  if (changed) renderAll(now);

  if (millis() - lastClock >= 1000) {
    lastClock = millis();
    struct tm lt;
    localtime_r(&now, &lt);
    char clock[8], date[20];
    strftime(clock, sizeof(clock), "%H:%M", &lt);
    strftime(date, sizeof(date), "%a %d %b", &lt);
    String d = date;
    d.toUpperCase();
    lv_label_set_text(nowClock, clock);
    lv_label_set_text(nowDate, d.c_str());
    lv_label_set_text(nowJalali, jalaliDate(lt).c_str());
    updateBanner(now);
    updateHeader(now, lt);
    if (lt.tm_sec == 0) {  // meeting countdown, "now" marker, agenda states
      renderNowTime(now);
      renderAgenda(now);
    }
  }

  if (millis() - lastSwitch >= pageDwellMs(page))
    showPage((page + 1) % PG_COUNT, true);
}

// Restarting is the recovery for a fragmented heap: everything is fetched again on boot.
static void restartFor(const char* why) {
  // Not in the first 10 minutes after boot: a cause a restart can't fix would loop forever.
  if (millis() < 600000UL) { LOG("restart skipped (just booted): %s", why); return; }
  LOG("restart: %s", why);
  Serial.flush();
  delay(200);
  ESP.restart();
}

// Any fetch that ran out of TLS memory (mbedTLS: "Memory allocation failed", ours: "no memory").
static bool memoryError(const String& e) {
  return e.indexOf("llocation") >= 0 || e.indexOf("memory") >= 0;
}

// Kuma check with the dashboard kept on screen: only the health data is copied and redrawn.
static void refreshKumaInPlace() {
  LOG("refresh: kuma in place");
  fetch_all(nullptr, REFRESH_KUMA);
  model_lock();
  snap.health = gData.health;
  seenVersion = gData.version;
  model_unlock();
  renderHealth();
  const HealthData& h = snap.health;
  if (h.count) {
    lv_label_set_text_fmt(hdHealth, "%s %d/%d", h.up == h.count ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, h.up, h.count);
    lv_obj_set_style_text_color(hdHealth, h.up == h.count ? C_LIME : C_RED, 0);
  }
  if (memoryError(h.err)) restartFor("kuma ran out of memory");
}

void ui_refresh(RefreshKind kind) {
  if (kind == REFRESH_KUMA && dashboardBuilt && ESP.getMaxAllocHeap() >= KUMA_INPLACE_BLOCK) {
    refreshKumaInPlace();
    return;
  }
  LOG("refresh: start (%s)", kind == REFRESH_FULL ? "full" : "kuma");
  started = false;
  styleBanner(BN_NONE);
  lv_obj_add_flag(header, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(tabBar, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(loadTitle, kind == REFRESH_FULL ? "SYNCING DATA" : "CHECKING SERVICES");
  for (int i = 0; i < STEP_COUNT; i++) onProgress((FetchStep)i, STEP_WAIT, "");
  lv_screen_load(loadScreen);
  // Free every dashboard object so the HTTPS handshakes get the RAM.
  if (dashboardBuilt) {
    for (int i = 0; i < PG_COUNT; i++) {
      lv_obj_delete(screens[i]);
      screens[i] = nullptr;
    }
    agList = nullptr;
    agLayout = -1;
    for (MeetSlot& m : agRows) m.box = nullptr;  // deleted with their screen
    dashboardBuilt = false;
  }
  // The UI copy of the data is rebuilt after the fetch; free its strings now as well.
  snap = Snapshot();
  lv_refr_now(nullptr);
  LOG("refresh: dashboard freed");
  if (ESP.getMaxAllocHeap() < MIN_TLS_BLOCK) restartFor("heap too fragmented for HTTPS");

  fetch_all(onProgress, kind);
  model_lock();
  bool outOfMemory = memoryError(gData.sprint.err) || memoryError(gData.health.err) || memoryError(gData.meet.err);
  model_unlock();
  if (outOfMemory) restartFor("a fetch ran out of memory");

  lv_label_set_text(loadTitle, LV_SYMBOL_OK " DATA READY");
  lv_refr_now(nullptr);
  uint32_t until = millis() + 1500;
  while (millis() < until) { lv_timer_handler(); delay(5); }

  buildDashboard();
  model_lock();
  snap = gData;
  seenVersion = gData.version;
  model_unlock();
  renderAll(time(nullptr));
  started = true;
  lv_obj_remove_flag(header, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(tabBar, LV_OBJ_FLAG_HIDDEN);
  hdPhraseText = "";
  lastClock = 0;
  showPage(PG_NOW, false);
  LOG("refresh: done, loop stack left %u bytes", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}
