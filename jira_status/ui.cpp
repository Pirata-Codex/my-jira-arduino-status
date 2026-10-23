#include "ui.h"
#include "config.h"
#include "theme.h"
#include "model.h"
#include "settings.h"
#include <WiFi.h>
#include "fetch.h"

enum Page { PG_NOW, PG_SPRINT, PG_CARDS, PG_KPI, PG_HEALTH, PG_COUNT };
static const char* PAGE_TITLES[PG_COUNT] = {"Now", "Sprint", "My cards", "KPIs", "Health"};

static Snapshot snap;                 // UI-side copy of gData
static uint32_t seenVersion = UINT32_MAX;
static lv_obj_t* screens[PG_COUNT];
static lv_obj_t* bootScreen;
static lv_obj_t *bootTitle, *bootDetail;
static int page = PG_NOW;
static uint32_t lastSwitch = 0, lastClock = 0;
static bool started = false;

// top bar
static lv_obj_t *topBar, *topTitle, *topClock, *topWifi, *topHealth, *dots[PG_COUNT];
// banner (meeting / outage)
static lv_obj_t *banner, *bannerIcon, *bannerText, *bannerSub;
static int bannerMode = -1;
// Now
static lv_obj_t *nowClock, *nowDate, *nowJalali, *nowMeetTitle, *nowMeetTime, *nowMeetWhere, *nowWorkKey, *nowWorkTitle, *nowWorkStatus, *nowMini;
// Sprint
static lv_obj_t *spArc, *spPts, *spPct, *spName, *spDays, *spBars[4], *spBarLbl[4], *spChart, *spFoot;
static lv_chart_series_t *spSeries, *spIdeal;
static lv_obj_t* spBurnNote;
// Cards
static lv_obj_t *cardView, *cardList;
static uint32_t cardsDwellMs = 0;  // how long the cards page needs to scroll through once
static const int CARD_VIEW_H = 232;
// KPIs
static lv_obj_t *kpiVal[6], *kpiLbl[6], *kpiSub[6];
// Health
static lv_obj_t *hlSummary, *hlGrid;

// ---------- small builders ----------

static lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t bg = C_CARD) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, 14, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, C_BORDER, 0);
  lv_obj_set_style_pad_all(o, 12, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* s = "") {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, s);
  return l;
}

static lv_obj_t* caption(lv_obj_t* parent, const char* s) {
  lv_obj_t* l = text(parent, F_SMALL, C_MUTED, s);
  lv_obj_set_style_text_letter_space(l, 1, 0);
  return l;
}

// Long messages (fetch errors) wrap instead of being cut off.
static lv_obj_t* errorText(lv_obj_t* parent, const char* s) {
  lv_obj_t* l = text(parent, F_BODY, C_MUTED, s);
  lv_obj_set_width(l, 440);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  return l;
}

static lv_obj_t* newScreen() {
  lv_obj_t* s = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s, C_BG, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static lv_color_t catColor(CardCat c, bool blocked) {
  if (blocked) return C_RED;
  switch (c) {
    case CAT_PROGRESS: return C_ACCENT;
    case CAT_REVIEW: return C_PURPLE;
    case CAT_DONE: return C_GREEN;
    default: return C_MUTED;
  }
}

static const char* catName(CardCat c) {
  static const char* n[] = {"To Do", "In Progress", "Review", "Done"};
  return n[c];
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

// Persian digits: U+06F0..U+06F9, two bytes each in UTF-8.
// LVGL's BiDi treats them as right-to-left letters and reverses the run, so emit them reversed.
static String faDigits(int n) {
  String in = String(n), out;
  for (int i = in.length() - 1; i >= 0; i--) {
    char ch = in[i];
    if (ch >= '0' && ch <= '9') {
      out += (char)0xDB;
      out += (char)(0xB0 + ch - '0');
    } else {
      out += ch;
    }
  }
  return out;
}

// Persian (Jalali) date, e.g. "جمعه ۱ آبان ۱۴۰۵".
static String jalaliDate(const struct tm& lt) {
  static const char* months[] = {"فروردین", "اردیبهشت", "خرداد", "تیر", "مرداد", "شهریور",
                                 "مهر", "آبان", "آذر", "دی", "بهمن", "اسفند"};
  static const char* weekdays[] = {"یکشنبه", "دوشنبه", "سه‌شنبه", "چهارشنبه", "پنجشنبه", "جمعه", "شنبه"};
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
  return String(weekdays[lt.tm_wday]) + " " + faDigits(jd) + " " + months[jm - 1] + " " + faDigits(jy);
}

static void pulseCb(void* obj, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t*)obj, (lv_opa_t)v, 0); }

// ---------- build ----------

static void buildTopBar() {
  lv_obj_t* top = lv_layer_top();
  topBar = lv_obj_create(top);
  lv_obj_remove_style_all(topBar);
  lv_obj_set_size(topBar, SCREEN_W, 36);
  lv_obj_set_style_pad_hor(topBar, 14, 0);
  lv_obj_remove_flag(topBar, LV_OBJ_FLAG_SCROLLABLE);

  topTitle = text(topBar, F_TITLE, C_TEXT, "Now");
  lv_obj_align(topTitle, LV_ALIGN_LEFT_MID, 0, 2);

  for (int i = 0; i < PG_COUNT; i++) {
    dots[i] = lv_obj_create(topBar);
    lv_obj_remove_style_all(dots[i]);
    lv_obj_set_size(dots[i], 6, 6);
    lv_obj_set_style_radius(dots[i], 3, 0);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
    lv_obj_align(dots[i], LV_ALIGN_CENTER, (i - PG_COUNT / 2) * 12, 2);
  }

  topClock = text(topBar, F_BODY, C_TEXT, "--:--");
  lv_obj_align(topClock, LV_ALIGN_RIGHT_MID, 0, 2);
  topWifi = text(topBar, F_SMALL, C_MUTED, LV_SYMBOL_WIFI);
  lv_obj_align(topWifi, LV_ALIGN_RIGHT_MID, -56, 2);
  topHealth = text(topBar, F_SMALL, C_GREEN, "");
  lv_obj_align(topHealth, LV_ALIGN_RIGHT_MID, -82, 2);
  lv_obj_add_flag(topBar, LV_OBJ_FLAG_HIDDEN);
}

static void buildBanner() {
  banner = box(lv_layer_top(), 10, SCREEN_H - 66, SCREEN_W - 20, 56, C_AMBER);
  lv_obj_set_style_border_width(banner, 0, 0);
  lv_obj_set_style_pad_hor(banner, 14, 0);
  lv_obj_set_style_pad_ver(banner, 6, 0);
  bannerIcon = text(banner, F_BIG, lv_color_black(), LV_SYMBOL_BELL);
  lv_obj_align(bannerIcon, LV_ALIGN_LEFT_MID, 0, 0);
  bannerText = text(banner, F_TITLE, lv_color_black(), "");
  lv_obj_set_width(bannerText, SCREEN_W - 100);
  lv_label_set_long_mode(bannerText, LV_LABEL_LONG_DOT);
  lv_obj_align(bannerText, LV_ALIGN_TOP_LEFT, 44, 0);
  bannerSub = text(banner, F_SMALL, lv_color_black(), "");
  lv_obj_set_width(bannerSub, SCREEN_W - 100);
  lv_label_set_long_mode(bannerSub, LV_LABEL_LONG_DOT);
  lv_obj_align(bannerSub, LV_ALIGN_BOTTOM_LEFT, 44, 0);
  lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

static void buildNow() {
  lv_obj_t* s = screens[PG_NOW] = newScreen();
  nowClock = text(s, F_HUGE, C_TEXT, "--:--");
  lv_obj_set_pos(nowClock, 18, 52);
  nowDate = text(s, F_BODY, C_MUTED, "");
  lv_obj_set_pos(nowDate, 20, 110);
  nowJalali = text(s, F_BODY, C_TEXT, "");
  lv_obj_set_pos(nowJalali, 20, 134);
  nowMini = text(s, F_SMALL, C_ACCENT, "");
  lv_obj_set_width(nowMini, 206);
  lv_label_set_long_mode(nowMini, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(nowMini, 20, 160);

  lv_obj_t* m = box(s, 236, 46, 230, 120);
  caption(m, LV_SYMBOL_BELL "  NEXT MEETING");
  nowMeetTitle = text(m, F_TITLE, C_TEXT, "");
  lv_obj_set_width(nowMeetTitle, 204);
  lv_label_set_long_mode(nowMeetTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(nowMeetTitle, 0, 24);
  nowMeetTime = text(m, F_BODY, C_AMBER, "");
  lv_obj_set_pos(nowMeetTime, 0, 52);
  nowMeetWhere = text(m, F_SMALL, C_MUTED, "");
  lv_obj_set_width(nowMeetWhere, 204);
  lv_label_set_long_mode(nowMeetWhere, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(nowMeetWhere, 0, 76);

  lv_obj_t* w = box(s, 14, 178, 452, 128);
  caption(w, LV_SYMBOL_PLAY "  WORKING ON");
  nowWorkKey = text(w, F_BIG, C_ACCENT, "");
  lv_obj_set_pos(nowWorkKey, 0, 24);
  nowWorkStatus = text(w, F_SMALL, C_ACCENT, "");
  lv_obj_align(nowWorkStatus, LV_ALIGN_TOP_RIGHT, 0, 0);
  nowWorkTitle = text(w, F_TITLE, C_TEXT, "");
  lv_obj_set_width(nowWorkTitle, 426);
  lv_label_set_long_mode(nowWorkTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(nowWorkTitle, 0, 66);
}

static void buildSprint() {
  lv_obj_t* s = screens[PG_SPRINT] = newScreen();
  lv_obj_t* left = box(s, 14, 46, 196, 260);

  spArc = lv_arc_create(left);
  lv_obj_set_size(spArc, 168, 168);
  lv_obj_align(spArc, LV_ALIGN_TOP_MID, 0, 0);
  lv_arc_set_bg_angles(spArc, 135, 45);
  lv_arc_set_range(spArc, 0, 100);
  lv_arc_set_value(spArc, 0);
  lv_obj_remove_style(spArc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(spArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(spArc, 14, LV_PART_MAIN);
  lv_obj_set_style_arc_color(spArc, C_BORDER, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spArc, 14, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spArc, C_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(spArc, true, LV_PART_INDICATOR);

  spPct = text(spArc, F_BIG, C_TEXT, "0%");
  lv_obj_align(spPct, LV_ALIGN_CENTER, 0, -8);
  lv_obj_t* l = caption(spArc, "POINTS DONE");
  lv_obj_align(l, LV_ALIGN_CENTER, 0, 20);
  spPts = text(left, F_TITLE, C_TEXT, "");
  lv_obj_align(spPts, LV_ALIGN_BOTTOM_MID, 0, -28);
  spFoot = text(left, F_SMALL, C_MUTED, "");
  lv_obj_align(spFoot, LV_ALIGN_BOTTOM_MID, 0, 0);

  lv_obj_t* right = box(s, 220, 46, 246, 260);
  spName = text(right, F_TITLE, C_TEXT, "");
  lv_obj_set_width(spName, 220);
  lv_label_set_long_mode(spName, LV_LABEL_LONG_DOT);
  spDays = text(right, F_SMALL, C_AMBER, "");
  lv_obj_set_pos(spDays, 0, 26);

  for (int i = 0; i < 4; i++) {
    int y = 52 + i * 24;
    spBarLbl[i] = text(right, F_SMALL, C_MUTED, "");
    lv_obj_set_pos(spBarLbl[i], 0, y);
    spBars[i] = lv_bar_create(right);
    lv_obj_set_size(spBars[i], 120, 8);
    lv_obj_set_pos(spBars[i], 100, y + 5);
    lv_obj_set_style_bg_color(spBars[i], C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(spBars[i], catColor((CardCat)i, false), LV_PART_INDICATOR);
  }

  lv_obj_t* bc = caption(right, "BURNDOWN");
  lv_obj_set_pos(bc, 0, 152);
  spChart = lv_chart_create(right);
  lv_obj_set_size(spChart, 222, 64);
  lv_obj_set_pos(spChart, 0, 172);
  lv_chart_set_type(spChart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(spChart, 0, 0);
  lv_obj_set_style_bg_opa(spChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spChart, 0, 0);
  lv_obj_set_style_pad_all(spChart, 2, 0);
  lv_obj_set_style_line_width(spChart, 3, LV_PART_ITEMS);
  lv_obj_set_style_size(spChart, 6, 6, LV_PART_INDICATOR);
  spIdeal = lv_chart_add_series(spChart, C_BORDER, LV_CHART_AXIS_PRIMARY_Y);
  spSeries = lv_chart_add_series(spChart, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
  spBurnNote = text(right, F_SMALL, C_MUTED, "");
  lv_obj_set_pos(spBurnNote, 0, 196);
}

static void legendItem(lv_obj_t* parent, const char* name, lv_color_t color) {
  lv_obj_t* item = lv_obj_create(parent);
  lv_obj_remove_style_all(item);
  lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(item, 6, 0);
  lv_obj_t* dot = lv_obj_create(item);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 10, 10);
  lv_obj_set_style_radius(dot, 3, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(dot, color, 0);
  text(item, F_SMALL, C_MUTED, name);
}

static void buildCards() {
  lv_obj_t* s = screens[PG_CARDS] = newScreen();
  lv_obj_t* legend = lv_obj_create(s);
  lv_obj_remove_style_all(legend);
  lv_obj_set_pos(legend, 16, 42);
  lv_obj_set_size(legend, 448, 22);
  lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  legendItem(legend, "To Do", catColor(CAT_TODO, false));
  legendItem(legend, "In Progress", catColor(CAT_PROGRESS, false));
  legendItem(legend, "Review", catColor(CAT_REVIEW, false));
  legendItem(legend, "Done", catColor(CAT_DONE, false));
  legendItem(legend, "Blocked", C_RED);

  // Clipping viewport; cardList moves up inside it like film credits.
  cardView = lv_obj_create(s);
  lv_obj_remove_style_all(cardView);
  lv_obj_set_pos(cardView, 14, 72);
  lv_obj_set_size(cardView, 452, CARD_VIEW_H);
  lv_obj_remove_flag(cardView, LV_OBJ_FLAG_SCROLLABLE);
  cardList = lv_obj_create(cardView);
  lv_obj_remove_style_all(cardList);
  lv_obj_set_size(cardList, 452, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(cardList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(cardList, 6, 0);
  lv_obj_remove_flag(cardList, LV_OBJ_FLAG_SCROLLABLE);
}

static void buildKpi() {
  lv_obj_t* s = screens[PG_KPI] = newScreen();
  for (int i = 0; i < 6; i++) {
    int col = i % 3, row = i / 3;
    lv_obj_t* t = box(s, 14 + col * 152, 46 + row * 132, 144, 124);
    kpiLbl[i] = caption(t, "");
    lv_obj_set_width(kpiLbl[i], 120);
    lv_label_set_long_mode(kpiLbl[i], LV_LABEL_LONG_DOT);
    kpiVal[i] = text(t, F_HUGE, C_TEXT, "-");
    lv_obj_align(kpiVal[i], LV_ALIGN_LEFT_MID, 0, 6);
    kpiSub[i] = text(t, F_SMALL, C_MUTED, "");
    lv_obj_align(kpiSub[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }
}

static void buildHealth() {
  lv_obj_t* s = screens[PG_HEALTH] = newScreen();
  hlSummary = text(s, F_SMALL, C_MUTED, "");
  lv_obj_align(hlSummary, LV_ALIGN_TOP_RIGHT, -16, 44);
  hlGrid = lv_obj_create(s);
  lv_obj_remove_style_all(hlGrid);
  lv_obj_set_pos(hlGrid, 14, 66);
  lv_obj_set_size(hlGrid, 452, 244);
  lv_obj_set_flex_flow(hlGrid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(hlGrid, 8, 0);
  lv_obj_set_style_pad_column(hlGrid, 8, 0);
  lv_obj_remove_flag(hlGrid, LV_OBJ_FLAG_SCROLLABLE);
}

// ---------- refresh screen ----------

static lv_obj_t* loadScreen;
static lv_obj_t *loadIcon[STEP_COUNT], *loadDetail[STEP_COUNT], *loadTitle;
static bool dashboardBuilt = false;

static void buildLoading() {
  static const char* names[STEP_COUNT] = {"Jira cards", "Sprint dates", "KPI queries", "Uptime Kuma", "Calendar"};
  loadScreen = newScreen();
  loadTitle = text(loadScreen, F_TITLE, C_TEXT, "Refreshing data");
  lv_obj_set_pos(loadTitle, 24, 22);
  for (int i = 0; i < STEP_COUNT; i++) {
    lv_obj_t* row = box(loadScreen, 20, 62 + i * 50, 440, 44);
    lv_obj_set_style_pad_ver(row, 0, 0);
    loadIcon[i] = text(row, F_TITLE, C_MUTED, LV_SYMBOL_DUMMY);
    lv_obj_align(loadIcon[i], LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* name = text(row, F_BODY, C_TEXT, names[i]);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 32, 0);
    loadDetail[i] = text(row, F_SMALL, C_MUTED, "");
    lv_obj_set_width(loadDetail[i], 250);
    lv_label_set_long_mode(loadDetail[i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(loadDetail[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(loadDetail[i], LV_ALIGN_RIGHT_MID, 0, 0);
  }
}

static void onProgress(FetchStep step, StepState state, const String& detail) {
  static const char* icons[] = {LV_SYMBOL_DUMMY, LV_SYMBOL_REFRESH, LV_SYMBOL_OK, LV_SYMBOL_CLOSE, LV_SYMBOL_MINUS};
  lv_color_t colors[] = {C_MUTED, C_ACCENT, C_GREEN, C_RED, C_MUTED};
  lv_label_set_text(loadIcon[step], icons[state]);
  lv_obj_set_style_text_color(loadIcon[step], colors[state], 0);
  lv_label_set_text(loadDetail[step], state == STEP_RUN ? "loading..." : detail.c_str());
  lv_obj_set_style_text_color(loadDetail[step], state == STEP_FAIL ? C_RED : C_MUTED, 0);
  lv_refr_now(nullptr);  // draw now: the next fetch blocks the loop
}

void ui_init() {
  bootScreen = newScreen();
  lv_obj_t* logo = text(bootScreen, F_HUGE, C_ACCENT, LV_SYMBOL_LIST);
  lv_obj_align(logo, LV_ALIGN_CENTER, 0, -60);
  bootTitle = text(bootScreen, F_TITLE, C_TEXT, "Jira Status");
  lv_obj_align(bootTitle, LV_ALIGN_CENTER, 0, 0);
  bootDetail = text(bootScreen, F_SMALL, C_MUTED, "");
  lv_obj_set_width(bootDetail, 420);
  lv_obj_set_style_text_align(bootDetail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(bootDetail, LV_ALIGN_CENTER, 0, 50);
  lv_screen_load(bootScreen);

  buildLoading();
  buildTopBar();
  buildBanner();
}

static void buildDashboard() {
  buildNow();
  buildSprint();
  buildCards();
  buildKpi();
  buildHealth();
  dashboardBuilt = true;
  bannerMode = -1;
  cardsDwellMs = 0;
}

void ui_boot(const String& title, const String& detail) {
  lv_label_set_text(bootTitle, title.c_str());
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

static const Meeting* nextMeeting(time_t now) {
  for (int i = 0; i < snap.meet.count; i++)
    if (snap.meet.m[i].end > now) return &snap.meet.m[i];
  return nullptr;
}

static void renderNow(time_t now) {
  const Meeting* m = nextMeeting(now);
  if (m) {
    lv_label_set_text(nowMeetTitle, m->title.c_str());
    long mins = (m->start - now) / 60;
    String when = hhmm(m->start) + " - " + hhmm(m->end);
    if (m->start > now) when += mins >= 60 ? "  ·  in " + String(mins / 60) + "h " + String(mins % 60) + "m" : "  ·  in " + String(mins) + " min";
    else when += "  ·  now";
    lv_label_set_text(nowMeetTime, when.c_str());
    lv_label_set_text(nowMeetWhere, m->where.c_str());
  } else {
    lv_label_set_text(nowMeetTitle, snap.meet.configured ? "No more meetings" : "Calendar not set");
    lv_label_set_text(nowMeetTime, snap.meet.configured ? "Enjoy the focus time" : "Add an .ics URL on the web page");
    lv_label_set_text(nowMeetWhere, "");
  }

  const Card* c = currentCard();
  lv_label_set_text(nowWorkKey, c ? c->key.c_str() : "Nothing in progress");
  lv_label_set_text(nowWorkTitle, c ? c->title.c_str() : "Pick a card from the sprint");
  lv_label_set_text(nowWorkStatus, c ? c->status.c_str() : "");
  if (c) lv_obj_set_style_text_color(nowWorkStatus, catColor(c->cat, c->blocked), 0);

  const SprintData& s = snap.sprint;
  if (s.ok && s.ptsTotal > 0) {
    String mini = String((int)(100 * s.ptsDone / s.ptsTotal)) + "% of sprint done";
    if (snap.mock) mini += "  (demo)";
    else if (s.updated) mini += "\nUpdated " + hhmm(s.updated);
    lv_label_set_text(nowMini, mini.c_str());
  } else {
    lv_label_set_text(nowMini, s.err.c_str());
  }
}

static void renderSprint(time_t now) {
  const SprintData& s = snap.sprint;
  int pct = s.ptsTotal > 0 ? (int)(100 * s.ptsDone / s.ptsTotal + 0.5f) : 0;
  lv_arc_set_value(spArc, pct);
  lv_label_set_text_fmt(spPct, "%d%%", pct);
  lv_label_set_text(spPts, (fmtPts(s.ptsDone) + " / " + fmtPts(s.ptsTotal) + " pts").c_str());
  lv_label_set_text(spName, s.name.c_str());
  if (s.endTs) {
    long days = (s.endTs - now + 86399) / 86400;
    lv_label_set_text(spDays, days > 0 ? (String(days) + (days == 1 ? " day left" : " days left")).c_str() : "Sprint ended");
    lv_obj_set_style_text_color(spDays, days <= 2 ? C_RED : C_AMBER, 0);
  } else {
    lv_label_set_text(spDays, "Set the board id for the end date");
  }

  int counts[4] = {}, blocked = 0;
  for (int i = 0; i < s.count; i++) {
    counts[s.cards[i].cat]++;
    if (s.cards[i].blocked) blocked++;
  }
  for (int i = 0; i < 4; i++) {
    lv_label_set_text_fmt(spBarLbl[i], "%s  %d", catName((CardCat)i), counts[i]);
    lv_bar_set_range(spBars[i], 0, max(1, s.count));
    lv_bar_set_value(spBars[i], counts[i], LV_ANIM_ON);
  }
  String foot = String(s.count) + " cards";
  if (blocked) foot += "  ·  " + String(blocked) + " blocked";
  lv_label_set_text(spFoot, foot.c_str());

  // Burndown from Jira itself: points remaining at the end of each sprint day,
  // from when each done card was resolved. Grey line = ideal.
  if (!s.startTs || !s.endTs || s.endTs <= s.startTs) {
    lv_obj_add_flag(spChart, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(spBurnNote, "Set the board id to see it");
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

static void creditsCb(void* obj, int32_t v) { lv_obj_set_y((lv_obj_t*)obj, v); }

// Scrolls the card list up through the viewport, pauses at the end, then glides back.
static void startCredits() {
  lv_anim_delete(cardList, creditsCb);
  lv_obj_set_y(cardList, 0);
  lv_obj_update_layout(cardList);
  int overflow = lv_obj_get_height(cardList) - CARD_VIEW_H;
  if (overflow <= 0) { cardsDwellMs = 0; return; }
  const uint32_t pxPerSec = 22, pause = 2000;
  uint32_t run = overflow * 1000 / pxPerSec;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, cardList);
  lv_anim_set_exec_cb(&a, creditsCb);
  lv_anim_set_values(&a, 0, -overflow);
  lv_anim_set_duration(&a, run);
  lv_anim_set_delay(&a, pause);
  lv_anim_set_playback_delay(&a, pause);
  lv_anim_set_playback_duration(&a, run / 3);
  lv_anim_set_repeat_delay(&a, pause);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_linear);
  lv_anim_start(&a);
  cardsDwellMs = pause + run + pause + run / 3;
}

static void renderCards() {
  lv_obj_clean(cardList);
  const SprintData& s = snap.sprint;
  // Active work first, done last.
  static const CardCat order[] = {CAT_PROGRESS, CAT_REVIEW, CAT_TODO, CAT_DONE};
  for (CardCat cat : order) {
    for (int i = 0; i < s.count; i++) {
      const Card& c = s.cards[i];
      if (c.cat != cat) continue;
      lv_color_t col = catColor(c.cat, c.blocked);
      // Tinted row in the status color.
      lv_obj_t* row = box(cardList, 0, 0, 452, 38, lv_color_mix(col, C_CARD, 48));
      lv_obj_set_style_pad_ver(row, 0, 0);
      lv_obj_set_style_radius(row, 10, 0);
      // Status stripe = thick left border (one object less per row; RAM is tight).
      lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
      lv_obj_set_style_border_width(row, 5, 0);
      lv_obj_set_style_border_color(row, col, 0);
      lv_obj_t* key = text(row, F_BODY, col, c.key.c_str());
      lv_obj_align(key, LV_ALIGN_LEFT_MID, 4, 0);
      lv_obj_t* title = text(row, F_BODY, c.cat == CAT_DONE ? C_MUTED : C_TEXT, c.title.c_str());
      lv_obj_set_width(title, 270);
      lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
      lv_obj_align(title, LV_ALIGN_LEFT_MID, 106, 0);
      lv_obj_t* pts = text(row, F_SMALL, C_TEXT, (fmtPts(c.pts) + " pt").c_str());
      lv_obj_align(pts, LV_ALIGN_RIGHT_MID, 0, 0);
    }
  }
  if (!s.count) errorText(cardList, s.ok ? "No cards in this sprint" : ("Jira: " + s.err).c_str());
  if (page == PG_CARDS) startCredits();
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
  setKpi(0, "POINTS DONE", fmtPts(s.ptsDone), "of " + fmtPts(s.ptsTotal) + " pts", C_GREEN);
  setKpi(1, "SPRINT SCOPE", String(pct) + "%", String(open) + " cards open", C_ACCENT);
  setKpi(2, "WIP", String(inProg + review), String(inProg) + " doing · " + String(review) + " review", inProg + review > 3 ? C_AMBER : C_TEXT);
  setKpi(3, "BLOCKED", String(blocked), blocked ? "needs attention" : "all clear", blocked ? C_RED : C_GREEN);
  String k1 = gCfg.kpi1Label, k2 = gCfg.kpi2Label;
  k1.toUpperCase();
  k2.toUpperCase();
  setKpi(4, k1, s.kpi1 < 0 ? "-" : String(s.kpi1), "Jira query", s.kpi1 > 0 ? C_AMBER : C_TEXT);
  setKpi(5, k2, s.kpi2 < 0 ? "-" : String(s.kpi2), "Jira query", s.kpi2 > 0 ? C_RED : C_GREEN);
}

static void renderHealth() {
  lv_obj_clean(hlGrid);
  const HealthData& h = snap.health;
  if (!h.ok && !h.count) {
    errorText(hlGrid, h.err.length() ? ("Kuma: " + h.err).c_str() : "Loading...");
    lv_label_set_text(hlSummary, "");
    return;
  }
  float upSum = 0;
  int upN = 0;
  for (int i = 0; i < h.count && i < 8; i++) {
    const Monitor& m = h.m[i];
    lv_obj_t* t = box(hlGrid, 0, 0, 222, 54);
    lv_obj_set_style_pad_ver(t, 6, 0);
    if (!m.up) lv_obj_set_style_border_color(t, C_RED, 0);
    lv_obj_t* dot = lv_obj_create(t);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, 6, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, m.up ? C_GREEN : C_RED, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* name = text(t, F_BODY, C_TEXT, m.name.c_str());
    lv_obj_set_width(name, 180);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 22, 0);
    String sub = m.up ? (m.ping >= 0 ? String(m.ping) + " ms" : String("up")) : String("DOWN");
    if (m.uptime >= 0) sub += "  ·  " + String(m.uptime * 100, 1) + "%";
    text(t, F_SMALL, m.up ? C_MUTED : C_RED, sub.c_str());
    lv_obj_align(lv_obj_get_child(t, -1), LV_ALIGN_BOTTOM_LEFT, 22, 0);
    if (m.uptime >= 0) { upSum += m.uptime; upN++; }
  }
  String sum = String(h.up) + "/" + String(h.count) + " up";
  if (upN) sum += "  ·  24h " + String(100 * upSum / upN, 2) + "%";
  lv_label_set_text(hlSummary, sum.c_str());
  lv_obj_set_style_text_color(hlSummary, h.up == h.count ? C_GREEN : C_RED, 0);
}

static void renderAll(time_t now) {
  LOG("render: %d cards, %d monitors, %d meetings", snap.sprint.count, snap.health.count, snap.meet.count);
  renderNow(now);
  renderSprint(now);
  renderCards();
  renderKpi();
  renderHealth();
  const HealthData& h = snap.health;
  if (h.count) {
    lv_label_set_text_fmt(topHealth, "%s %d/%d", h.up == h.count ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, h.up, h.count);
    lv_obj_set_style_text_color(topHealth, h.up == h.count ? C_GREEN : C_RED, 0);
  }
}

// ---------- banner: meeting soon / meeting started / outage ----------

enum { BN_NONE, BN_SOON, BN_STARTED, BN_DOWN };

static void showBanner(int mode, const String& title, const String& sub) {
  if (mode == BN_NONE) {
    if (bannerMode != BN_NONE) {
      lv_anim_delete(banner, pulseCb);
      lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    }
    bannerMode = BN_NONE;
    return;
  }
  lv_label_set_text(bannerText, title.c_str());
  lv_label_set_text(bannerSub, sub.c_str());
  if (mode == bannerMode) return;
  bannerMode = mode;
  lv_obj_remove_flag(banner, LV_OBJ_FLAG_HIDDEN);
  lv_anim_delete(banner, pulseCb);
  lv_color_t bg = mode == BN_SOON ? C_AMBER : C_RED;
  lv_color_t fg = mode == BN_SOON ? lv_color_black() : lv_color_white();
  lv_obj_set_style_bg_color(banner, bg, 0);
  lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
  for (lv_obj_t* l : {bannerIcon, bannerText, bannerSub}) lv_obj_set_style_text_color(l, fg, 0);
  lv_label_set_text(bannerIcon, mode == BN_DOWN ? LV_SYMBOL_WARNING : LV_SYMBOL_BELL);
  if (mode != BN_SOON) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, banner);
    lv_anim_set_values(&a, 150, 255);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_playback_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, pulseCb);
    lv_anim_start(&a);
  }
}

static void updateBanner(time_t now) {
  // A meeting that just started wins, then an outage, then a meeting about to start.
  for (int i = 0; i < snap.meet.count; i++) {
    const Meeting& m = snap.meet.m[i];
    long since = now - m.start;
    if (since >= 0 && since < MEETING_ALERT_SEC) {
      char b[32];
      snprintf(b, sizeof(b), "Started %ld:%02ld ago", since / 60, since % 60);
      showBanner(BN_STARTED, m.title, String(b) + (m.where.length() ? "  ·  " + m.where : ""));
      return;
    }
  }
  const HealthData& h = snap.health;
  if (h.ok && h.up < h.count) {
    String names;
    for (int i = 0; i < h.count; i++)
      if (!h.m[i].up) names += (names.length() ? ", " : "") + h.m[i].name;
    showBanner(BN_DOWN, String(h.count - h.up) + (h.count - h.up == 1 ? " service down" : " services down"), names);
    return;
  }
  for (int i = 0; i < snap.meet.count; i++) {
    const Meeting& m = snap.meet.m[i];
    long until = m.start - now;
    if (until > 0 && until <= MEETING_SOON_SEC) {
      long mins = (until + 59) / 60;
      showBanner(BN_SOON, m.title, "Starts in " + String(mins) + " min at " + hhmm(m.start) + (m.where.length() ? "  ·  " + m.where : ""));
      return;
    }
  }
  showBanner(BN_NONE, "", "");
}

// ---------- loop ----------

static void showPage(int p, bool animate) {
  LOG("page %s", PAGE_TITLES[p]);
  page = p;
  lv_screen_load_anim(screens[p], animate ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_NONE, animate ? 350 : 0, 0, false);
  lv_label_set_text(topTitle, PAGE_TITLES[p]);
  for (int i = 0; i < PG_COUNT; i++) {
    lv_obj_set_style_bg_color(dots[i], i == p ? C_ACCENT : C_BORDER, 0);
    lv_obj_set_width(dots[i], i == p ? 16 : 6);
  }
  if (p == PG_CARDS) startCredits();
  lastSwitch = millis();
}

// The cards page stays until its list has scrolled through once.
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
    char clock[8], date[32];
    strftime(clock, sizeof(clock), "%H:%M", &lt);
    strftime(date, sizeof(date), "%A, %d %B", &lt);
    lv_label_set_text(topClock, clock);
    lv_label_set_text(nowClock, clock);
    lv_label_set_text(nowDate, date);
    lv_label_set_text(nowJalali, jalaliDate(lt).c_str());
    bool wifi = WiFi.status() == WL_CONNECTED;
    lv_obj_set_style_text_color(topWifi, wifi ? C_MUTED : C_RED, 0);
    updateBanner(now);
    if (lt.tm_sec == 0) renderNow(now);  // meeting countdown
  }

  if (millis() - lastSwitch >= pageDwellMs(page))
    showPage((page + 1) % PG_COUNT, true);
}

void ui_refresh() {
  LOG("refresh: start");
  started = false;
  showBanner(BN_NONE, "", "");
  lv_obj_add_flag(topBar, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(loadTitle, "Refreshing data");
  for (int i = 0; i < STEP_COUNT; i++) onProgress((FetchStep)i, STEP_WAIT, "");
  lv_screen_load(loadScreen);
  // Free every dashboard object so the HTTPS handshakes get the RAM.
  if (dashboardBuilt) {
    for (int i = 0; i < PG_COUNT; i++) {
      lv_obj_delete(screens[i]);
      screens[i] = nullptr;
    }
    dashboardBuilt = false;
  }
  lv_refr_now(nullptr);
  LOG("refresh: dashboard freed");

  fetch_all(onProgress);

  lv_label_set_text(loadTitle, LV_SYMBOL_OK "  Data ready");
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
  lv_obj_remove_flag(topBar, LV_OBJ_FLAG_HIDDEN);
  showPage(PG_NOW, false);
  LOG("refresh: done, loop stack left %u bytes", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

