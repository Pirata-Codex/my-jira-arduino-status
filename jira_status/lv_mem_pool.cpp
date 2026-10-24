// LVGL's memory (lv_conf.h: LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM).
//
// The screen and the HTTPS connections used to share one heap. Every dashboard rebuild left small
// LVGL blocks scattered through it, and after a while TLS couldn't find the ~17 KB pieces it needs
// ("SSL - Memory allocation failed"). Here LVGL gets its own region, reserved once at boot before
// WiFi starts, so its churn stays inside that region. When the region is full, LVGL falls back to
// the normal heap instead of failing, so a too-small pool costs fragmentation, never a crash.
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <multi_heap.h>
#include <string.h>
#include "config.h"

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "Old lv_conf.h: copy extras/lv_conf.h next to the lvgl library folder again (it now sets LV_STDLIB_CUSTOM)."
#endif

// These are declared in LVGL's C sources, so they need C linkage here.
extern "C" {

static uint8_t* poolMem = nullptr;
static multi_heap_handle_t pool = nullptr;
static size_t overflowAllocs = 0;  // allocations that didn't fit the pool (logged)

static bool inPool(const void* p) {
  return poolMem && (const uint8_t*)p >= poolMem && (const uint8_t*)p < poolMem + LVGL_POOL_BYTES;
}

void lv_mem_init(void) {
  poolMem = (uint8_t*)heap_caps_malloc(LVGL_POOL_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (poolMem) pool = multi_heap_register(poolMem, LVGL_POOL_BYTES);
  if (!pool) LOG("lvgl pool: could not reserve %u bytes, using the heap", (unsigned)LVGL_POOL_BYTES);
}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t p) { LV_UNUSED(p); }

void* lv_malloc_core(size_t size) {
  void* p = pool ? multi_heap_malloc(pool, size) : nullptr;
  if (!p) {
    p = malloc(size);
    if (p) overflowAllocs++;
  }
  return p;
}

void lv_free_core(void* p) {
  if (!p) return;
  if (inPool(p)) multi_heap_free(pool, p);
  else free(p);
}

void* lv_realloc_core(void* p, size_t size) {
  if (!p) return lv_malloc_core(size);
  if (!inPool(p)) return realloc(p, size);
  void* n = multi_heap_realloc(pool, p, size);
  if (n || !size) return n;
  // Doesn't fit in the pool any more: move it to the heap.
  n = malloc(size);
  if (!n) return nullptr;
  size_t old = multi_heap_get_allocated_size(pool, p);
  memcpy(n, p, old < size ? old : size);
  multi_heap_free(pool, p);
  overflowAllocs++;
  return n;
}

void lv_mem_monitor_core(lv_mem_monitor_t* m) {
  memset(m, 0, sizeof(*m));
  if (!pool) return;
  multi_heap_info_t i;
  multi_heap_get_info(pool, &i);
  m->total_size = LVGL_POOL_BYTES;
  m->free_size = i.total_free_bytes;
  m->free_biggest_size = i.largest_free_block;
  m->free_cnt = i.free_blocks;
  m->used_cnt = i.allocated_blocks;
  m->max_used = LVGL_POOL_BYTES - i.minimum_free_bytes;
  m->used_pct = 100 - 100 * i.total_free_bytes / LVGL_POOL_BYTES;
  m->frag_pct = i.total_free_bytes ? 100 - 100 * i.largest_free_block / i.total_free_bytes : 0;
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
