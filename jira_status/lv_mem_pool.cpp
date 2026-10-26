// LVGL's memory (lv_conf.h: LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM).
//
// The screen and the HTTPS connections used to share one heap. Every dashboard rebuild left small
// LVGL blocks scattered through it, and after a while TLS couldn't find the ~17 KB pieces it needs
// ("SSL - Memory allocation failed"). Here LVGL gets its own memory, reserved once at boot before
// WiFi starts, so its churn stays there. When it is full, LVGL falls back to the normal heap
// instead of failing, so a too-small pool costs fragmentation, never a crash.
//
// The pool is reserved in POOL_CHUNKS pieces, taken from the SMALLER heap regions: the ESP32's
// biggest region is what HTTPS needs its large blocks from, and one 56 KB pool cut out of it left
// a largest free block of 33 KB, too small for TLS (Kuma and Jira failed). While the chunks are
// reserved, the biggest region is held by a temporary blocker so they land elsewhere.
#include <Arduino.h>  // Serial, millis, ESP for LOG (a .cpp file doesn't get it automatically)
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <multi_heap.h>
#include <string.h>
#include "config.h"

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "Old lv_conf.h: copy extras/lv_conf.h next to the lvgl library folder again (it now sets LV_STDLIB_CUSTOM)."
#endif

static const int POOL_CHUNKS = 4;
static const size_t CHUNK_BYTES = LVGL_POOL_BYTES / POOL_CHUNKS;

// These are declared in LVGL's C sources, so they need C linkage here.
extern "C" {

static uint8_t* chunkMem[POOL_CHUNKS];
static multi_heap_handle_t chunk[POOL_CHUNKS];
static int chunkCount = 0;
static size_t overflowAllocs = 0;  // allocations that didn't fit the pool (logged)

static int chunkOf(const void* p) {
  for (int i = 0; i < chunkCount; i++)
    if ((const uint8_t*)p >= chunkMem[i] && (const uint8_t*)p < chunkMem[i] + CHUNK_BYTES) return i;
  return -1;
}

void lv_mem_init(void) {
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  size_t before = heap_caps_get_largest_free_block(caps);
  // Hold the biggest free block so the chunks come from the other regions.
  void* blocker = heap_caps_malloc(before, caps);
  for (int i = 0; i < POOL_CHUNKS; i++) {
    uint8_t* m = (uint8_t*)heap_caps_malloc(CHUNK_BYTES, caps);
    if (!m) break;
    multi_heap_handle_t h = multi_heap_register(m, CHUNK_BYTES);
    if (!h) { heap_caps_free(m); break; }
    chunkMem[chunkCount] = m;
    chunk[chunkCount++] = h;
  }
  if (blocker) heap_caps_free(blocker);
  // Whatever didn't fit elsewhere comes from the big region after all.
  while (chunkCount < POOL_CHUNKS) {
    uint8_t* m = (uint8_t*)heap_caps_malloc(CHUNK_BYTES, caps);
    multi_heap_handle_t h = m ? multi_heap_register(m, CHUNK_BYTES) : nullptr;
    if (!h) { if (m) heap_caps_free(m); break; }
    chunkMem[chunkCount] = m;
    chunk[chunkCount++] = h;
  }
  LOG("lvgl pool: %d x %u KB, largest free block %u KB before, %u KB after", chunkCount,
      (unsigned)(CHUNK_BYTES / 1024), (unsigned)(before / 1024), (unsigned)(heap_caps_get_largest_free_block(caps) / 1024));
}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t p) { LV_UNUSED(p); }

void* lv_malloc_core(size_t size) {
  for (int i = 0; i < chunkCount; i++) {
    void* p = multi_heap_malloc(chunk[i], size);
    if (p) return p;
  }
  void* p = malloc(size);
  if (p) overflowAllocs++;
  return p;
}

void lv_free_core(void* p) {
  if (!p) return;
  int c = chunkOf(p);
  if (c >= 0) multi_heap_free(chunk[c], p);
  else free(p);
}

void* lv_realloc_core(void* p, size_t size) {
  if (!p) return lv_malloc_core(size);
  int c = chunkOf(p);
  if (c < 0) return realloc(p, size);
  void* n = multi_heap_realloc(chunk[c], p, size);
  if (n || !size) return n;
  // Doesn't fit in its chunk any more: move it (another chunk or the heap).
  n = lv_malloc_core(size);
  if (!n) return nullptr;
  size_t old = multi_heap_get_allocated_size(chunk[c], p);
  memcpy(n, p, old < size ? old : size);
  multi_heap_free(chunk[c], p);
  return n;
}

void lv_mem_monitor_core(lv_mem_monitor_t* m) {
  memset(m, 0, sizeof(*m));
  size_t total = 0, freeB = 0, minFree = 0, biggest = 0;
  for (int i = 0; i < chunkCount; i++) {
    multi_heap_info_t info;
    multi_heap_get_info(chunk[i], &info);
    total += CHUNK_BYTES;
    freeB += info.total_free_bytes;
    minFree += info.minimum_free_bytes;
    if (info.largest_free_block > biggest) biggest = info.largest_free_block;
    m->free_cnt += info.free_blocks;
    m->used_cnt += info.allocated_blocks;
  }
  if (!total) return;
  m->total_size = total;
  m->free_size = freeB;
  m->free_biggest_size = biggest;
  m->max_used = total - minFree;
  m->used_pct = 100 - 100 * freeB / total;
  m->frag_pct = freeB ? 100 - 100 * biggest / freeB : 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"

// For the heartbeat log: pool use and how often LVGL spilled into the heap.
void lvgl_pool_stats(unsigned& usedKb, unsigned& peakKb, unsigned& overflow) {
  lv_mem_monitor_t m;
  lv_mem_monitor_core(&m);
  usedKb = (m.total_size - m.free_size) / 1024;
  peakKb = m.max_used / 1024;
  overflow = overflowAllocs;
}
