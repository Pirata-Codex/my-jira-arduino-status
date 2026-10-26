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
//
// Two places hold memory (lv_mem_pool.cpp): the ARENA (LVGL_POOL_BYTES, reserved before WiFi,
// shared by the screen and the HTTPS buffers) and the HEAP (everything else: JSON, data text,
// sockets, web server).

enum MemFeature : uint8_t {
  // Transient: one HTTPS request (TLS in the arena when it fits, JSON and the rest on the heap).
  MF_FETCH_STATUS, MF_FETCH_CALENDAR, MF_FETCH_JIRA, MF_FETCH_KUMA,
  // Arena, alive while shown: one dashboard page, the status screen.
  MF_PAGE_NOW, MF_PAGE_AGENDA, MF_PAGE_SPRINT, MF_PAGE_CARDS, MF_PAGE_KPI, MF_PAGE_HEALTH, MF_STATUS_SCREEN,
  MF_COUNT
};
enum MemPlace : uint8_t { MP_FETCH, MP_SCREEN };

struct MemBudget {
  const char* name;
  MemPlace place;
  uint32_t bytes;  // estimated peak use (fetch: heap + arena together)
};

// ----- TLS, from the ESP32 core's mbedTLS configuration -----
// CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN 16384, not asymmetric, no variable buffers: every connection
// allocates an input and an output record buffer of 16384 + ~330 bytes, each in one piece.
constexpr uint32_t TLS_BUF = 17 * 1024;
constexpr int TLS_BUFS = 2;
// What a connection allocates in mbedTLS: the two buffers plus context, handshake and certificates.
constexpr uint32_t TLS_EST = 42 * 1024;
// The part of a fetch that stays on the heap whatever happens (JSON document, HTTP client, URL
// text): the heaviest fetch (Jira) minus its TLS part.
constexpr uint32_t FETCH_HEAP_MIN = 24 * 1024;

// ----- estimates (bytes), from the /memory measurements of 2026-10-26 -----
// Always on screen: header, tab bar, banner, LVGL's own state. The refresh screen exists only
// during a refresh (when the dashboard is freed).
constexpr uint32_t UI_BASE_EST = 22 * 1024;
constexpr uint32_t REFRESH_SCREEN_EST = 5 * 1024;

constexpr MemBudget MEM_BUDGET[MF_COUNT] = {
    {"fetch status", MP_FETCH, 60 * 1024},
    {"fetch calendar", MP_FETCH, 59 * 1024},
    {"fetch jira", MP_FETCH, 66 * 1024},  // TLS + a 30-issue JSON document
    {"fetch kuma", MP_FETCH, 53 * 1024},
    {"page pulse", MP_SCREEN, 16 * 1024},
    {"page agenda", MP_SCREEN, 8 * 1024},
    {"page sprint", MP_SCREEN, 10 * 1024},
    {"page cards", MP_SCREEN, 9 * 1024},
    {"page kpi", MP_SCREEN, 6 * 1024},
    {"page health", MP_SCREEN, 11 * 1024},
    {"status screen", MP_SCREEN, 12 * 1024},
};

// ----- the plan -----
// Free heap once WiFi runs (byte-addressable RAM, arena and draw buffer already reserved):
// measured 71 KB with a 56 KB pool, so ~55 KB with the 72 KB arena. mem_init() logs the real value.
constexpr uint32_t HEAP_BASE_EST = 55 * 1024;
// Fixed heap users besides the fetches: web server, OTA, mDNS, sockets.
constexpr uint32_t HEAP_SERVICES_EST = 8 * 1024;
// Text of one data snapshot (card titles, meeting names, monitor names) lives on the heap; the
// structs themselves are globals. Two copies (gData and the UI's): a fetch moves its result in.
constexpr uint32_t SNAPSHOT_TEXT_EST = MAX_CARDS * 110 + MAX_MONITORS * 40 + (MAX_MEETINGS + MAX_FUTURE) * 110 + 600;
constexpr uint32_t SNAPSHOT_FRESH_EST = sizeof(SprintData);  // the biggest `new T()` in publish()

constexpr uint32_t memMax(MemPlace place, int i = 0) {
  return i >= MF_COUNT ? 0
       : (MEM_BUDGET[i].place == place && MEM_BUDGET[i].bytes > memMax(place, i + 1)) ? MEM_BUDGET[i].bytes
       : memMax(place, i + 1);
}

// A fetch is TLS (arena) + its heap part.
static_assert(memMax(MP_FETCH) <= TLS_EST + FETCH_HEAP_MIN, "Memory plan: a fetch estimate exceeds TLS_EST + FETCH_HEAP_MIN");
// Heap: what stays, plus the heap part of the heaviest fetch.
constexpr uint32_t HEAP_PLAN = HEAP_SERVICES_EST + 2 * SNAPSHOT_TEXT_EST + SNAPSHOT_FRESH_EST + FETCH_HEAP_MIN;
static_assert(HEAP_PLAN <= HEAP_BASE_EST, "Memory plan: the heap part of a fetch doesn't fit (see memory.h / docs/MEMORY.md)");
// Arena during a refresh: base UI + refresh screen + the rest of the heaviest fetch.
constexpr uint32_t ARENA_REFRESH_PLAN = UI_BASE_EST + REFRESH_SCREEN_EST + TLS_EST;
static_assert(ARENA_REFRESH_PLAN <= LVGL_POOL_BYTES, "Memory plan: a refresh doesn't fit the arena");
// The screen fills chunk 0 (32 KB) during a refresh, chunks 1 and 2 (20 KB) take one TLS buffer each.
static_assert(UI_BASE_EST + REFRESH_SCREEN_EST <= 32 * 1024 && TLS_BUF <= 20 * 1024 - 256,
              "Memory plan: the arena chunks no longer leave a piece per TLS buffer");
// Arena with the dashboard: base UI plus two pages at once (the slide shows the old and the new page).
constexpr uint32_t ARENA_UI_PLAN = UI_BASE_EST + 2 * memMax(MP_SCREEN);
static_assert(ARENA_UI_PLAN <= LVGL_POOL_BYTES, "Memory plan: base UI + two pages don't fit the arena");
// While a status is set the status screen alternates with HEALTH.
static_assert(UI_BASE_EST + MEM_BUDGET[MF_STATUS_SCREEN].bytes + MEM_BUDGET[MF_PAGE_HEALTH].bytes <= LVGL_POOL_BYTES,
              "Memory plan: status screen + health page don't fit the arena");

// ----- run time -----
void mem_init();                     // after WiFi: TLS into the arena, failed-allocation hook, learned values
bool mem_admit(MemFeature f);        // is there room for f right now? (logs a refusal)
uint32_t mem_need(MemFeature f);     // bytes f is expected to need (learned, else the estimate)
void mem_begin(MemFeature f);        // measure f from here...
void mem_end(MemFeature f, bool ok, const String& err = "");  // ...to here
void mem_save();                     // store learned values (call when idle, e.g. after a refresh)
void mem_reset_learned();
String mem_report_html();            // the /memory page
bool mem_is_oom(const String& err);  // an error text that means "ran out of memory"
