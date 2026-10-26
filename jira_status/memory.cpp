#include "memory.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <Preferences.h>
#include <mbedtls/platform.h>

// The arena (lv_mem_pool.cpp).
size_t lvgl_pool_free_bytes();
size_t lvgl_used_bytes();
int arena_slots(size_t bytes);
void* arena_tls_calloc(size_t n, size_t size);
void arena_tls_free(void* p);

static const uint32_t HEAP_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
static const int TRUST_SAMPLES = 3;      // measured values replace the estimate after this many runs
static const uint32_t SLACK = 2 * 1024;  // added to every need, plus 15%

// What the board learned about each feature (kept in NVS, key "mem2").
struct Learned {
  uint32_t peak;  // highest measured use (fetch: heap + arena)
  uint16_t samples;
  uint16_t fails;  // runs that ran out of memory
};
static Learned learned[MF_COUNT];
static bool dirty = false;
static uint32_t heapBase = 0, blockBase = 0;  // after WiFi (logged; compare with HEAP_BASE_EST)

// Current measurement.
static int scopeFeature = -1;
static uint32_t scopeFreeBefore, scopeUsedBefore;
static volatile uint32_t scopeMinFree;
static esp_timer_handle_t sampler = nullptr;
// Failed allocations (from ESP-IDF), attributed to the feature running at the time.
static volatile uint32_t failSize = 0, failCount = 0;
static volatile int failFeature = -1;

static uint32_t freeHeap() { return heap_caps_get_free_size(HEAP_CAPS); }
static uint32_t largestBlock() { return heap_caps_get_largest_free_block(HEAP_CAPS); }
// A fetch can use the heap and the arena's free space.
static uint32_t freeForFetch() { return freeHeap() + lvgl_pool_free_bytes(); }

// Separate free pieces big enough for a TLS buffer: in the arena, plus the heap's largest block.
static int tlsSlots() {
  uint32_t b = largestBlock();
  return arena_slots(TLS_BUF) + (b >= 2 * TLS_BUF + 64 ? 2 : b >= TLS_BUF ? 1 : 0);
}

// The low point during a fetch: TLS allocates and frees inside the library, so free memory is
// sampled every 4 ms while a fetch runs.
static void sampleCb(void*) {
  uint32_t f = freeForFetch();
  if (f < scopeMinFree) scopeMinFree = f;
}

static void failedAllocCb(size_t size, uint32_t caps, const char* fn) {
  (void)caps;
  (void)fn;
  failSize = size;
  failCount++;
  failFeature = scopeFeature;
}

static void load() {
  Preferences p;
  if (!p.begin("mem", true)) return;
  if (p.getBytesLength("mem2") == sizeof(learned)) p.getBytes("mem2", learned, sizeof(learned));
  p.end();
}

void mem_save() {
  if (!dirty) return;
  Preferences p;
  if (!p.begin("mem", false)) return;
  p.putBytes("mem2", learned, sizeof(learned));
  p.end();
  dirty = false;
}

void mem_reset_learned() {
  memset(learned, 0, sizeof(learned));
  dirty = true;
  mem_save();
  LOG("mem: learned values reset, using the estimates");
}

void mem_init() {
  load();
  // From here on every TLS allocation tries the arena first (after WiFi: the company WiFi login
  // also uses mbedTLS, and its long-lived state should stay on the heap).
  mbedtls_platform_set_calloc_free(arena_tls_calloc, arena_tls_free);
  heap_caps_register_failed_alloc_callback(failedAllocCb);
  esp_timer_create_args_t a = {};
  a.callback = sampleCb;
  a.name = "memsample";
  esp_timer_create(&a, &sampler);
  heapBase = freeHeap();
  blockBase = largestBlock();
  LOG("mem: after wifi %uk heap free, largest block %uk (plan assumes %uk); arena %uk free, %d TLS slots",
      (unsigned)(heapBase / 1024), (unsigned)(blockBase / 1024), (unsigned)(HEAP_BASE_EST / 1024),
      (unsigned)(lvgl_pool_free_bytes() / 1024), tlsSlots());
}

bool mem_is_oom(const String& e) { return e.indexOf("llocation") >= 0 || e.indexOf("memory") >= 0; }

uint32_t mem_need(MemFeature f) {
  const Learned& l = learned[f];
  uint32_t base = l.samples >= TRUST_SAMPLES ? l.peak : max(MEM_BUDGET[f].bytes, l.peak);
  return base + base * 15 / 100 + SLACK;
}

// Fetch: enough free memory overall, two pieces for the TLS buffers, and the heap part on the heap.
// Screen: enough free arena.
static bool room(MemFeature f, uint32_t& have) {
  if (MEM_BUDGET[f].place == MP_FETCH) {
    have = freeForFetch();
    return have >= mem_need(f) && tlsSlots() >= TLS_BUFS && freeHeap() >= FETCH_HEAP_MIN;
  }
  have = lvgl_pool_free_bytes();
  return have >= mem_need(f);
}

bool mem_admit(MemFeature f) {
  uint32_t have;
  bool ok = room(f, have);
  if (!ok) {
    if (MEM_BUDGET[f].place == MP_FETCH)
      LOG("mem: no room for %s: needs %uk, free %uk (heap %uk), %d of %d TLS slots", MEM_BUDGET[f].name,
          (unsigned)(mem_need(f) / 1024), (unsigned)(have / 1024), (unsigned)(freeHeap() / 1024), tlsSlots(), TLS_BUFS);
    else
      LOG("mem: no room for %s: needs %uk, arena free %uk", MEM_BUDGET[f].name, (unsigned)(mem_need(f) / 1024),
          (unsigned)(have / 1024));
  }
  return ok;
}

void mem_begin(MemFeature f) {
  scopeFeature = f;
  if (MEM_BUDGET[f].place == MP_FETCH) {
    scopeFreeBefore = scopeMinFree = freeForFetch();
    if (sampler) esp_timer_start_periodic(sampler, 4000);
  } else {
    scopeUsedBefore = lvgl_used_bytes();
  }
}

void mem_end(MemFeature f, bool ok, const String& err) {
  if (scopeFeature != f) return;
  scopeFeature = -1;
  Learned& l = learned[f];
  uint32_t used;
  if (MEM_BUDGET[f].place == MP_FETCH) {
    if (sampler) esp_timer_stop(sampler);
    sampleCb(nullptr);
    used = scopeFreeBefore > scopeMinFree ? scopeFreeBefore - scopeMinFree : 0;
    bool oom = mem_is_oom(err) || failFeature == f;
    failFeature = -1;
    if (oom) {
      if (l.fails < 0xFFFF) l.fails++;
      dirty = true;
    }
    if (!ok) {  // a failed fetch ended early: its use says little about a full run
      LOG("mem: %s failed (%s) after using %uk", MEM_BUDGET[f].name, oom ? "out of memory" : "other error",
          (unsigned)(used / 1024));
      return;
    }
  } else {
    uint32_t now = lvgl_used_bytes();
    used = now > scopeUsedBefore ? now - scopeUsedBefore : 0;
  }
  if (used > l.peak + 512) dirty = true;  // only notable changes are written to flash
  if (used > l.peak) l.peak = used;
  if (l.samples < 0xFFFF) l.samples++;
  if (l.samples == TRUST_SAMPLES) dirty = true;
  LOG("mem: %s used %uk (estimate %uk, learned peak %uk)", MEM_BUDGET[f].name, (unsigned)(used / 1024),
      (unsigned)(MEM_BUDGET[f].bytes / 1024), (unsigned)(l.peak / 1024));
}

String mem_report_html() {
  String h;
  h.reserve(3200);
  h += "<h3>Memory</h3><p>Heap " + String(freeHeap() / 1024) + " KB free, largest block " + String(largestBlock() / 1024) +
       " KB, lowest since boot " + String(heap_caps_get_minimum_free_size(HEAP_CAPS) / 1024) + " KB. After WiFi: " +
       String(heapBase / 1024) + " KB (plan assumes " + String(HEAP_BASE_EST / 1024) + ").<br>Arena " +
       String(LVGL_POOL_BYTES / 1024) + " KB: screen " + String(lvgl_used_bytes() / 1024) + " KB, free " +
       String(lvgl_pool_free_bytes() / 1024) + " KB. TLS buffer slots now: " + String(tlsSlots()) + " (needs " +
       String(TLS_BUFS) + ").<br>Plan: heap " + String(HEAP_PLAN / 1024) + " / " + String(HEAP_BASE_EST / 1024) +
       " KB, arena during refresh " + String(ARENA_REFRESH_PLAN / 1024) + " / " + String(LVGL_POOL_BYTES / 1024) +
       " KB, arena with the dashboard " + String(ARENA_UI_PLAN / 1024) + " KB.";
  if (failCount) h += "<br>Failed allocations: " + String(failCount) + ", last " + String(failSize) + " bytes.";
  h += "</p><table><tr><th>Feature</th><th>Uses</th><th>Estimate</th><th>Measured peak</th><th>Runs</th>"
       "<th>Needs now</th><th>Out of memory</th><th>Room now</th></tr>";
  for (int i = 0; i < MF_COUNT; i++) {
    MemFeature f = (MemFeature)i;
    const Learned& l = learned[i];
    uint32_t have;
    bool r = room(f, have);
    h += "<tr><td>" + String(MEM_BUDGET[i].name) + "</td><td>" + (MEM_BUDGET[i].place == MP_FETCH ? "heap + arena" : "arena") +
         "</td><td>" + String(MEM_BUDGET[i].bytes / 1024) + " KB</td><td>" +
         (l.samples ? String(l.peak / 1024) + " KB" : String("-")) + "</td><td>" + String(l.samples) + "</td><td>" +
         String(mem_need(f) / 1024) + " KB</td><td>" + String(l.fails) + "</td><td>" + (r ? "yes" : "<b>no</b>") + "</td></tr>";
  }
  h += "</table><p><small>Screen features are measured while their page is built, fetches only when they "
       "succeed. <a href='/memory?reset=1'>Forget measurements</a></small></p>";
  return h;
}
