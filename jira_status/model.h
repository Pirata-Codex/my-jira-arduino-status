#pragma once
#include <Arduino.h>
#include <time.h>

enum CardCat : uint8_t { CAT_TODO, CAT_PROGRESS, CAT_REVIEW, CAT_DONE };

constexpr int MAX_CARDS = 30;
constexpr int MAX_MONITORS = 12;
constexpr int MAX_MEETINGS = 10;

struct Card {
  String key, title, status;
  CardCat cat = CAT_TODO;
  float pts = 0;
  bool blocked = false;
  time_t resolved = 0;        // done cards: when they were resolved (burndown)
};

struct SprintData {
  bool ok = false;
  String err;
  String name;
  time_t startTs = 0, endTs = 0;  // 0 when no board id is configured
  float ptsDone = 0, ptsTotal = 0;
  int count = 0;
  Card cards[MAX_CARDS];
  int kpi1 = -1, kpi2 = -1;   // custom JQL counts, -1 = not configured / failed
  time_t updated = 0;
};

struct Monitor {
  String name;
  bool up = true;
  int ping = -1;
  float uptime = -1;          // 24h, 0..1
};

struct HealthData {
  bool ok = false;
  String err;
  int count = 0, up = 0;
  Monitor m[MAX_MONITORS];
  time_t updated = 0;
};

struct Meeting {
  String title, where;
  time_t start = 0, end = 0;
  uint32_t uid = 0;        // hash of the event UID (parsing only: matches moved/cancelled occurrences)
  bool fromRule = false;   // an occurrence of a repeating event
};

struct MeetingData {
  bool ok = false;
  bool configured = false;
  String err;
  int count = 0;
  Meeting m[MAX_MEETINGS];    // today's meetings, sorted by start
  time_t updated = 0;
};

struct Snapshot {
  SprintData sprint;
  HealthData health;
  MeetingData meet;
  bool mock = false;
  uint32_t version = 0;       // bumped on every change; the UI redraws when it moves
};

// Written by the fetch task, read by the UI task. Always access under model_lock().
extern Snapshot gData;
void model_init();
void model_lock();
void model_unlock();
void model_fill_mock(Snapshot& s);

