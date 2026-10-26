#include "memory.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <Preferences.h>

// LVGL's memory (lv_mem_pool.cpp): free bytes in the pool, and LVGL's total use (pool + heap overflow).
size_t lvgl_pool_free_bytes();
size_t lvgl_used_bytes();

static const uint32_t HEAP_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
static const int TRUST_SAMPLES = 3;      // measured values replace the estimate after this many runs
static const uint32_t SLACK = 2 * 1024;  // added to every need, plus 15%

// What the board learned about each feature (kept in NVS, key "mem").
struct Learned {
  uint32_t peak;       // highest measured use
  uint32_t okBlock;    // smallest largest-free-block it worked with (heap)
  uint32_t failBlock;  // largest largest-free-block it ran out of memory with (heap)
  uint16_t samples;
  uint16_t fails;
};
static Learned learned[MF_COUNT];
static bool dirty = false;
static uint32_t heapBase = 0, blockBase = 0;  // after WiFi (logged; compare with HEAP_BASE_EST)

// Current measurement.
static int scopeFeature = -1;
static uint32_t scopeFreeBefore, scopeBlockBefore, scopeUsedBefore;
static volatile uint32_t scopeMinFree;
static esp_timer_handle_t sampler = nullptr;
// Failed allocations (from ESP-IDF), attributed to the feature running at the time.
static volatile uint32_t failSize = 0, failCount = 0;
static volatile int failFeature = -1;

static uint32_t freeHeap() { return heap_caps_get_free_size(HEAP_CAPS); }
static uint32_t largestBlock() { return heap_caps_get_largest_free_block(HEAP_CAPS); }

// The low point of free heap during a fetch: TLS allocates and frees inside the library, so it is
// sampled every 4 ms while a heap feature runs.
static void sampleCb(void*) {
  uint32_t f = freeHeap();
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
  if (p.getBytesLength("learned") == sizeof(learned)) p.getBytes("learned", learned, sizeof(learned));
  p.end();
}

void mem_save() {
  if (!dirty) return;
  Preferences p;
  if (!p.begin("mem", false)) return;
  p.putBytes("learned", learned, sizeof(learned));
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
  heap_caps_register_failed_alloc_callback(failedAllocCb);
  esp_timer_create_args_t a = {};
  a.callback = sampleCb;
  a.name = "memsample";
  esp_timer_create(&a, &sampler);
  heapBase = freeHeap();
  blockBase = largestBlock();
  LOG("mem: after wifi %uk free, largest block %uk (plan assumes %uk); plan heap %uk, pool %uk of %uk",
      (unsigned)(heapBase / 1024), (unsigned)(blockBase / 1024), (unsigned)(HEAP_BASE_EST / 1024),
      (unsigned)(HEAP_PLAN / 1024), (unsigned)(POOL_PLAN / 1024), (unsigned)(LVGL_POOL_BYTES / 1024));
}

bool mem_is_oom(const String& e) { return e.indexOf("llocation") >= 0 || e.indexOf("memory") >= 0; }

uint32_t mem_need(MemFeature f) {
  const Learned& l = learned[f];
  uint32_t base = l.samples >= TRUST_SAMPLES ? l.peak : max(MEM_BUDGET[f].bytes, l.peak);
  return base + base * 15 / 100 + SLACK;
}

// Largest block a heap feature needs: above any block it has failed with, and (once measured)
// no more than the smallest it has worked with.
static uint32_t blockNeed(MemFeature f) {
  const Learned& l = learned[f];
  uint32_t need = MEM_BUDGET[f].block;
  if (l.samples >= TRUST_SAMPLES && l.okBlock && l.okBlock < need) need = l.okBlock;
  if (l.failBlock && need <= l.failBlock) need = l.failBlock + 1024;
  return need;
}

bool mem_admit(MemFeature f) {
  bool ok;
  uint32_t need = mem_need(f), have, block = 0, blockReq = 0;
  if (MEM_BUDGET[f].place == MP_HEAP) {
    have = freeHeap();
    block = largestBlock();
    blockReq = blockNeed(f);
    ok = have >= need && block >= blockReq;
  } else {
    have = lvgl_pool_free_bytes();
    ok = have >= need;
  }
  if (!ok)
    LOG("mem: no room for %s: needs %uk (block %uk), free %uk (block %uk)", MEM_BUDGET[f].name,
        (unsigned)(need / 1024), (unsigned)(blockReq / 1024), (unsigned)(have / 1024), (unsigned)(block / 1024));
  return ok;
}

void mem_begin(MemFeature f) {
  scopeFeature = f;
  if (MEM_BUDGET[f].place == MP_HEAP) {
    scopeFreeBefore = scopeMinFree = freeHeap();
    scopeBlockBefore = largestBlock();
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
  if (MEM_BUDGET[f].place == MP_HEAP) {
    if (sampler) esp_timer_stop(sampler);
    sampleCb(nullptr);
    used = scopeFreeBefore > scopeMinFree ? scopeFreeBefore - scopeMinFree : 0;
    bool oom = mem_is_oom(err) || failFeature == f;
    if (oom) {
      if (scopeBlockBefore > l.failBlock) l.failBlock = scopeBlockBefore;
      if (l.fails < 0xFFFF) l.fails++;
      dirty = true;
    } else if (ok && (!l.okBlock || scopeBlockBefore < l.okBlock)) {
      l.okBlock = scopeBlockBefore;
      dirty = true;
    }
    failFeature = -1;
    if (!ok) {  // a failed fetch ended early: its use says little about a full run
      LOG("mem: %s failed (%s), block was %uk", MEM_BUDGET[f].name, oom ? "out of memory" : "other error",
          (unsigned)(scopeBlockBefore / 1024));
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
  h.reserve(3000);
  h += "<h3>Memory</h3><p>Heap " + String(freeHeap() / 1024) + " KB free, largest block " + String(largestBlock() / 1024) +
       " KB, lowest since boot " + String(heap_caps_get_minimum_free_size(HEAP_CAPS) / 1024) + " KB. After WiFi: " +
       String(heapBase / 1024) + " KB (plan assumes " + String(HEAP_BASE_EST / 1024) + ").<br>LVGL " +
       String(lvgl_used_bytes() / 1024) + " KB used, pool " + String(lvgl_pool_free_bytes() / 1024) + " KB free of " +
       String(LVGL_POOL_BYTES / 1024) + ".<br>Plan: heap " + String(HEAP_PLAN / 1024) + " / " + String(HEAP_BASE_EST / 1024) +
       " KB, pool " + String(POOL_PLAN / 1024) + " / " + String(LVGL_POOL_BYTES / 1024) + " KB.";
  if (failCount) h += "<br>Failed allocations: " + String(failCount) + ", last " + String(failSize) + " bytes.";
  h += "</p><table><tr><th>Feature</th><th>Where</th><th>Estimate</th><th>Measured peak</th><th>Runs</th>"
       "<th>Needs now</th><th>Block</th><th>OOM</th><th>Room now</th></tr>";
  for (int i = 0; i < MF_COUNT; i++) {
    MemFeature f = (MemFeature)i;
    const Learned& l = learned[i];
    bool heap = MEM_BUDGET[i].place == MP_HEAP;
    bool room = heap ? freeHeap() >= mem_need(f) && largestBlock() >= blockNeed(f) : lvgl_pool_free_bytes() >= mem_need(f);
    h += "<tr><td>" + String(MEM_BUDGET[i].name) + "</td><td>" + (heap ? "heap" : "pool") + "</td><td>" +
         String(MEM_BUDGET[i].bytes / 1024) + " KB</td><td>" + (l.samples ? String(l.peak / 1024) + " KB" : String("-")) +
         "</td><td>" + String(l.samples) + "</td><td>" + String(mem_need(f) / 1024) + " KB</td><td>" +
         (heap ? String(blockNeed(f) / 1024) + " KB" : String("-")) + "</td><td>" + String(l.fails) + "</td><td>" +
         (room ? "yes" : "<b>no</b>") + "</td></tr>";
  }
  h += "</table><p><small>Pool features are counted while their page is built. Heap features are only "
       "measured when they succeed. <a href='/memory?reset=1'>Forget measurements</a></small></p>";
  return h;
}
