// The memory arena: one block of RAM reserved at boot and shared by the screen (LVGL) and HTTPS
// (mbedTLS). lv_conf.h: LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM; memory.cpp redirects mbedTLS here.
//
// Why shared: on this board (no PSRAM) one HTTPS connection needs two ~17 KB buffers, each in one
// piece (CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN 16384, symmetric). Once WiFi is up, the company WiFi
// login (PEAP) has broken the heap into pieces no bigger than ~33 KB, so those buffers often can't
// be found there. The arena is reserved before WiFi starts, while the memory is still whole, in
// three pieces:
//
//   [ chunk 0: 32 KB ][ chunk 1: 20 KB ][ chunk 2: 20 KB ]
//     LVGL fills from the left  ->           <- mbedTLS fills from the right
//
// During a refresh the dashboard is freed, so the screen holds only chunk 0 and each 17 KB TLS
// buffer gets a 20 KB piece of its own. With the dashboard on screen the pages may reach into
// chunk 1; HTTPS then takes what is left and falls back to the heap (memory.cpp decides whether a
// fetch can run at all). Anything that doesn't fit the arena falls back to the normal heap, so a
// full arena costs fragmentation, never a crash.
#include <Arduino.h>  // Serial, millis, ESP for LOG (a .cpp file doesn't get it automatically)
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <multi_heap.h>
#include <string.h>
#include "config.h"

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "Old lv_conf.h: copy extras/lv_conf.h next to the lvgl library folder again (it now sets LV_STDLIB_CUSTOM)."
#endif

static const int ARENA_CHUNKS = 3;
static const size_t CHUNK_BYTES[ARENA_CHUNKS] = {32 * 1024, 20 * 1024, 20 * 1024};
static_assert(32 * 1024 + 20 * 1024 + 20 * 1024 == LVGL_POOL_BYTES, "LVGL_POOL_BYTES must equal the arena chunks");

static const uint32_t CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

static uint8_t* chunkMem[ARENA_CHUNKS];
static multi_heap_handle_t chunk[ARENA_CHUNKS];
static int chunkCount = 0;
// mbedTLS also runs in the WiFi task (company WiFi login), so arena calls are serialized.
static portMUX_TYPE arenaMux = portMUX_INITIALIZER_UNLOCKED;
static size_t overflowAllocs = 0;  // LVGL allocations that didn't fit the arena (logged)
static size_t overflowLive = 0;    // bytes of those still allocated (LVGL's use of the heap)
static size_t tlsLive = 0;         // bytes mbedTLS holds in the arena right now

static int chunkOf(const void* p) {
  for (int i = 0; i < chunkCount; i++)
    if ((const uint8_t*)p >= chunkMem[i] && (const uint8_t*)p < chunkMem[i] + CHUNK_BYTES[i]) return i;
  return -1;
}

static void* chunkMalloc(int i, size_t size) {
  portENTER_CRITICAL(&arenaMux);
  void* p = multi_heap_malloc(chunk[i], size);
  portEXIT_CRITICAL(&arenaMux);
  return p;
}

static size_t chunkFree(int c, void* p) {
  portENTER_CRITICAL(&arenaMux);
  size_t n = multi_heap_get_allocated_size(chunk[c], p);
  multi_heap_free(chunk[c], p);
  portEXIT_CRITICAL(&arenaMux);
  return n;
}

// These are declared in LVGL's C sources, so they need C linkage here.
extern "C" {

void lv_mem_init(void) {
  size_t before = heap_caps_get_largest_free_block(CAPS);
  // Biggest chunk first, while the memory is still whole.
  for (int i = 0; i < ARENA_CHUNKS; i++) {
    uint8_t* m = (uint8_t*)heap_caps_malloc(CHUNK_BYTES[i], CAPS);
    multi_heap_handle_t h = m ? multi_heap_register(m, CHUNK_BYTES[i]) : nullptr;
    if (!h) {
      if (m) heap_caps_free(m);
      break;
    }
    chunkMem[chunkCount] = m;
    chunk[chunkCount++] = h;
  }
  LOG("arena: %d chunks (32+20+20 KB), largest free block %u KB before, %u KB after", chunkCount,
      (unsigned)(before / 1024), (unsigned)(heap_caps_get_largest_free_block(CAPS) / 1024));
}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t p) { LV_UNUSED(p); }

void* lv_malloc_core(size_t size) {
  for (int i = 0; i < chunkCount; i++) {  // from the left
    void* p = chunkMalloc(i, size);
    if (p) return p;
  }
  void* p = heap_caps_malloc(size, CAPS);
  if (p) {
    overflowAllocs++;
    overflowLive += heap_caps_get_allocated_size(p);
  }
  return p;
}

void lv_free_core(void* p) {
  if (!p) return;
  int c = chunkOf(p);
  if (c >= 0) {
    chunkFree(c, p);
    return;
  }
  size_t n = heap_caps_get_allocated_size(p);
  overflowLive = overflowLive > n ? overflowLive - n : 0;
  heap_caps_free(p);
}

void* lv_realloc_core(void* p, size_t size) {
  if (!p) return lv_malloc_core(size);
  int c = chunkOf(p);
  if (c < 0) {
    size_t before = heap_caps_get_allocated_size(p);
    void* n = heap_caps_realloc(p, size, CAPS);
    if (n || !size) {
      overflowLive = overflowLive > before ? overflowLive - before : 0;
      if (n) overflowLive += heap_caps_get_allocated_size(n);
    }
    return n;
  }
  portENTER_CRITICAL(&arenaMux);
  void* n = multi_heap_realloc(chunk[c], p, size);
  portEXIT_CRITICAL(&arenaMux);
  if (n || !size) return n;
  // Doesn't fit in its chunk any more: move it (another chunk or the heap).
  n = lv_malloc_core(size);
  if (!n) return nullptr;
  portENTER_CRITICAL(&arenaMux);
  size_t old = multi_heap_get_allocated_size(chunk[c], p);
  portEXIT_CRITICAL(&arenaMux);
  memcpy(n, p, old < size ? old : size);
  chunkFree(c, p);
  return n;
}

void lv_mem_monitor_core(lv_mem_monitor_t* m) {
  memset(m, 0, sizeof(*m));
  size_t total = 0, freeB = 0, minFree = 0, biggest = 0;
  for (int i = 0; i < chunkCount; i++) {
    multi_heap_info_t info;
    portENTER_CRITICAL(&arenaMux);
    multi_heap_get_info(chunk[i], &info);
    portEXIT_CRITICAL(&arenaMux);
    total += CHUNK_BYTES[i];
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

// ----- mbedTLS side (installed by memory.cpp with mbedtls_platform_set_calloc_free) -----

void* arena_tls_calloc(size_t n, size_t size) {
  size_t bytes = n * size;
  if (size && bytes / size != n) return nullptr;
  for (int i = chunkCount - 1; i >= 0; i--) {  // from the right
    void* p = chunkMalloc(i, bytes);
    if (p) {
      memset(p, 0, bytes);
      portENTER_CRITICAL(&arenaMux);
      tlsLive += multi_heap_get_allocated_size(chunk[i], p);
      portEXIT_CRITICAL(&arenaMux);
      return p;
    }
  }
  return heap_caps_calloc(n, size, CAPS);  // what ESP-IDF's own mbedTLS allocator does
}

void arena_tls_free(void* p) {
  if (!p) return;
  int c = chunkOf(p);
  if (c < 0) {
    heap_caps_free(p);  // allocated on the heap (or before the hook was installed)
    return;
  }
  size_t n = chunkFree(c, p);
  portENTER_CRITICAL(&arenaMux);
  tlsLive = tlsLive > n ? tlsLive - n : 0;
  portEXIT_CRITICAL(&arenaMux);
}

// ----- numbers for the memory budget (memory.cpp) and the heartbeat -----

size_t lvgl_pool_free_bytes() {
  lv_mem_monitor_t m;
  lv_mem_monitor_core(&m);
  return m.free_size;
}

// LVGL's own use: arena minus free minus what mbedTLS holds, plus its overflow into the heap.
size_t lvgl_used_bytes() {
  lv_mem_monitor_t m;
  lv_mem_monitor_core(&m);
  size_t used = m.total_size - m.free_size;
  return (used > tlsLive ? used - tlsLive : 0) + overflowLive;
}

// How many separate free pieces of at least `bytes` the arena has (one per chunk at most counted
// twice when a chunk could hold two): TLS needs two buffers of ~17 KB.
int arena_slots(size_t bytes) {
  int n = 0;
  for (int i = 0; i < chunkCount; i++) {
    multi_heap_info_t info;
    portENTER_CRITICAL(&arenaMux);
    multi_heap_get_info(chunk[i], &info);
    portEXIT_CRITICAL(&arenaMux);
    if (info.largest_free_block >= bytes) n += info.largest_free_block >= 2 * bytes + 64 ? 2 : 1;
  }
  return n;
}

void lvgl_pool_stats(unsigned& usedKb, unsigned& peakKb, unsigned& overflow) {
  lv_mem_monitor_t m;
  lv_mem_monitor_core(&m);
  usedKb = (m.total_size - m.free_size) / 1024;
  peakKb = m.max_used / 1024;
  overflow = overflowAllocs;
}
