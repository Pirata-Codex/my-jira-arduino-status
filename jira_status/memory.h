#pragma once
#include <Arduino.h>
#include "config.h"
#include "model.h"

// Memory budget and admission control. See docs/MEMORY.md for the model and the procedure for
// adding a feature.
//
// Every memory-hungry feature is listed below with an estimate of its peak use. Three things use
// that list:
//   1. The PLAN (compile time): static_asserts check that the estimates fit the board. A feature
//      that doesn't fit stops the build with a message instead of crashing on the desk.
//   2. MEASUREMENT (run time): every time a feature runs, its real peak is measured and learned
//      (kept in NVS across restarts). The /memory page shows estimate vs measured.
//   3. ADMISSION (run time): before a feature runs, mem_admit() compares what it needs (measured
//      when known, else the estimate) with what is free right now, and the caller frees memory,
//      postpones or restarts instead of running into "out of memory".

enum MemFeature : uint8_t {
  // Heap, transient: one HTTPS request (TLS + response parsing), freed when it ends.
  MF_FETCH_STATUS, MF_FETCH_CALENDAR, MF_FETCH_JIRA, MF_FETCH_KUMA,
  // LVGL pool, alive while shown: one dashboard page, the status screen.
  MF_PAGE_NOW, MF_PAGE_AGENDA, MF_PAGE_SPRINT, MF_PAGE_CARDS, MF_PAGE_KPI, MF_PAGE_HEALTH, MF_STATUS_SCREEN,
  MF_COUNT
};
enum MemPlace : uint8_t { MP_HEAP, MP_POOL };

struct MemBudget {
  const char* name;
  MemPlace place;
  uint32_t bytes;  // estimated peak use
  uint32_t block;  // heap only: largest contiguous block it needs (TLS buffers)
};

// ----- estimates (bytes) -----
// Heap: from the serial logs (free heap before/while fetching). TLS needs one block of ~40 KB:
// handshakes failed with a largest free block of 33 KB and worked from 38-40 KB.
constexpr uint32_t TLS_BLOCK = 40 * 1024;
// LVGL pool: UI that always exists (header, tab bar, banner, refresh screen, LVGL's own state).
constexpr uint32_t UI_BASE_EST = 30 * 1024;

constexpr MemBudget MEM_BUDGET[MF_COUNT] = {
    {"fetch status", MP_HEAP, 46 * 1024, TLS_BLOCK},
    {"fetch calendar", MP_HEAP, 54 * 1024, TLS_BLOCK},
    {"fetch jira", MP_HEAP, 60 * 1024, TLS_BLOCK},  // TLS + a 30-issue JSON document
    {"fetch kuma", MP_HEAP, 52 * 1024, TLS_BLOCK},
    {"page pulse", MP_POOL, 9 * 1024, 0},
    {"page agenda", MP_POOL, 6 * 1024, 0},
    {"page sprint", MP_POOL, 8 * 1024, 0},
    {"page cards", MP_POOL, 7 * 1024, 0},
    {"page kpi", MP_POOL, 5 * 1024, 0},
    {"page health", MP_POOL, 7 * 1024, 0},
    {"status screen", MP_POOL, 9 * 1024, 0},
};

// ----- the plan -----
// Free heap once WiFi runs, with the LVGL pool and draw buffer already reserved and the dashboard
// freed (measured: ~104-110 KB). mem_init() logs the real value; update this when it changes.
constexpr uint32_t HEAP_BASE_EST = 104 * 1024;
// Fixed heap users besides the fetches: web server, OTA, mDNS, sockets.
constexpr uint32_t HEAP_SERVICES_EST = 8 * 1024;
// Text of one data snapshot (card titles, meeting names, monitor names) lives on the heap; the
// structs themselves are globals. There are up to three copies: the fetch's fresh one, gData, the UI's.
constexpr uint32_t SNAPSHOT_TEXT_EST = MAX_CARDS * 110 + MAX_MONITORS * 40 + (MAX_MEETINGS + MAX_FUTURE) * 110 + 600;
constexpr uint32_t SNAPSHOT_FRESH_EST = sizeof(SprintData);  // the biggest `new T()` in publish()

constexpr uint32_t memMax(MemPlace place, int i = 0) {
  return i >= MF_COUNT ? 0
       : (MEM_BUDGET[i].place == place && MEM_BUDGET[i].bytes > memMax(place, i + 1)) ? MEM_BUDGET[i].bytes
       : memMax(place, i + 1);
}

// Heap: the heaviest fetch must fit next to everything that stays.
constexpr uint32_t HEAP_PLAN = HEAP_SERVICES_EST + 3 * SNAPSHOT_TEXT_EST + SNAPSHOT_FRESH_EST + memMax(MP_HEAP);
static_assert(HEAP_PLAN <= HEAP_BASE_EST,
              "Memory plan: the heaviest fetch no longer fits the heap (see memory.h / docs/MEMORY.md)");
// Pool: base UI plus two pages at once (the slide animation shows the old and the new page).
constexpr uint32_t POOL_PLAN = UI_BASE_EST + 2 * memMax(MP_POOL);
static_assert(POOL_PLAN <= LVGL_POOL_BYTES,
              "Memory plan: base UI + two pages don't fit LVGL_POOL_BYTES (see memory.h / docs/MEMORY.md)");
// While a status is set the status screen alternates with HEALTH.
static_assert(UI_BASE_EST + MEM_BUDGET[MF_STATUS_SCREEN].bytes + MEM_BUDGET[MF_PAGE_HEALTH].bytes <= LVGL_POOL_BYTES,
              "Memory plan: status screen + health page don't fit LVGL_POOL_BYTES");

// ----- run time -----
void mem_init();                     // after WiFi: baseline, failed-allocation hook, learned values
bool mem_admit(MemFeature f);        // is there room for f right now? (logs a refusal)
uint32_t mem_need(MemFeature f);     // bytes f is expected to need (learned, else the estimate)
void mem_begin(MemFeature f);        // measure f from here...
void mem_end(MemFeature f, bool ok, const String& err = "");  // ...to here
void mem_save();                     // store learned values (call when idle, e.g. after a refresh)
void mem_reset_learned();
String mem_report_html();            // the /memory page
bool mem_is_oom(const String& err);  // an error text that means "ran out of memory"
