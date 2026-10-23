#include "model.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

Snapshot gData;
static SemaphoreHandle_t mtx;

void model_init() { mtx = xSemaphoreCreateMutex(); }
void model_lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
void model_unlock() { xSemaphoreGive(mtx); }

static SemaphoreHandle_t netMtx = xSemaphoreCreateMutex();
void net_lock() { xSemaphoreTake(netMtx, portMAX_DELAY); }
void net_unlock() { xSemaphoreGive(netMtx); }
bool net_try_lock() { return xSemaphoreTake(netMtx, 0) == pdTRUE; }

static void card(SprintData& s, const char* key, const char* title, const char* status, CardCat cat, float pts, bool blocked = false, int doneDaysAgo = 0) {
  if (s.count >= MAX_CARDS) return;
  Card& c = s.cards[s.count++];
  c.key = key; c.title = title; c.status = status; c.cat = cat; c.pts = pts; c.blocked = blocked;
  if (cat == CAT_DONE) c.resolved = time(nullptr) - doneDaysAgo * 86400L;
  s.ptsTotal += pts;
  if (cat == CAT_DONE) s.ptsDone += pts;
}

// Demo data so the screens can be checked before Jira is configured.
void model_fill_mock(Snapshot& d) {
  time_t now = time(nullptr);
  SprintData& s = d.sprint;
  s = SprintData();
  s.ok = true;
  s.name = "CRM Sprint 42";
  s.startTs = now - 9 * 86400;
  s.endTs = now + 4 * 86400;
  card(s, "CRM-854", "Sync medrep visits to BP", "In Progress", CAT_PROGRESS, 5);
  card(s, "CRM-861", "Contact birthdate cleanup", "In Review", CAT_REVIEW, 3);
  card(s, "CRM-870", "SMS campaign report", "To Do", CAT_TODO, 8);
  card(s, "CRM-872", "Plan lock Hangfire job", "Blocked", CAT_TODO, 3, true);
  card(s, "CRM-840", "Department % fix", "Done", CAT_DONE, 5, false, 7);
  card(s, "CRM-843", "اصلاح گزارش ویزیت‌ها", "Done", CAT_DONE, 8, false, 4);
  card(s, "CRM-848", "AD group mapping", "Done", CAT_DONE, 2, false, 1);
  card(s, "UBP-710", "قفل برنامه‌ها بعد از پایان ماه", "In Progress", CAT_PROGRESS, 5);
  card(s, "UBP-692", "Hangfire retry policy", "To Do", CAT_TODO, 1);
  card(s, "CRM-3229", "خروجی اکسل پیامک‌ها", "To Do", CAT_TODO, 2);
  card(s, "CRM-3230", "Fix contact merge rule", "Done", CAT_DONE, 0.5, false, 2);
  s.kpi1 = 3;
  s.kpi2 = 1;
  s.updated = now;

  HealthData& h = d.health;
  h = HealthData();
  h.ok = true;
  const char* names[] = {"CRM Web", "CRM API", "BP Portal", "Hangfire", "SQL Gateway", "Data8 Sync"};
  for (int i = 0; i < 6; i++) {
    Monitor& m = h.m[h.count++];
    m.name = names[i];
    m.up = i != 4;
    m.ping = 80 + i * 37;
    m.uptime = i == 4 ? 0.962f : 0.999f;
    if (m.up) h.up++;
  }
  h.updated = now;

  MeetingData& mt = d.meet;
  mt = MeetingData();
  mt.ok = mt.configured = true;
  mt.count = 2;
  mt.m[0].title = "Daily stand-up";
  mt.m[0].where = "Teams";
  mt.m[0].start = now + 6 * 60;
  mt.m[0].end = mt.m[0].start + 15 * 60;
  mt.m[1].title = "Sprint review";
  mt.m[1].where = "Room 3";
  mt.m[1].start = now + 3 * 3600;
  mt.m[1].end = mt.m[1].start + 3600;
  mt.updated = now;
  d.mock = true;
}
