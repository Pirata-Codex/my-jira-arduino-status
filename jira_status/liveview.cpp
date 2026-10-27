#include "liveview.h"
#include "config.h"
#include "sound.h"
#include <esp_heap_caps.h>

// Live view. Every strip LVGL sends to the display is also copied into a mirror of the screen in
// PSRAM (a memcpy, the display never waits for the network). A task on core 0 sends the changed
// rectangles to the browser, at most 10 times a second, plus the room sound level once a second.
// Internal RAM: the task stack (3 KB, created with the first viewer) and the socket's send buffer
// while someone watches. Without PSRAM there's no mirror and /stream says so.
//
// Wire format (little endian), after the HTTP headers:
//   "FRDY" u16 width u16 height
//   then packets: u8 1, u16 x y w h, w*h RGB565 pixels   (a changed rectangle)
//                 u8 2, u16 dB*10, i8 stage               (room sound, stage -1 = no microphone)

static uint16_t* mirror = nullptr;  // W x H RGB565
static int W = 0, H = 0;
static volatile bool live = false;
static QueueHandle_t handoff = nullptr;  // WiFiClient* for the task

struct Rect { int16_t x1, y1, x2, y2; };
static const int MAX_RECTS = 8;
static Rect dirty[MAX_RECTS];
static int dirtyN = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static bool touches(const Rect& a, const Rect& b) {
  return a.x1 <= b.x2 + 1 && b.x1 <= a.x2 + 1 && a.y1 <= b.y2 + 1 && b.y1 <= a.y2 + 1;
}

static void unite(Rect& a, const Rect& b) {
  a.x1 = min(a.x1, b.x1);
  a.y1 = min(a.y1, b.y1);
  a.x2 = max(a.x2, b.x2);
  a.y2 = max(a.y2, b.y2);
}

static void markDirty(const Rect& r) {
  portENTER_CRITICAL(&mux);
  int i = 0;
  while (i < dirtyN && !touches(dirty[i], r)) i++;
  if (i < dirtyN) unite(dirty[i], r);                 // strips of one area join up
  else if (dirtyN < MAX_RECTS) dirty[dirtyN++] = r;
  else unite(dirty[MAX_RECTS - 1], r);
  portEXIT_CRITICAL(&mux);
}

void stream_capture(const lv_area_t* a, const uint8_t* px) {
  if (!live) return;
  int x1 = max(0, (int)a->x1), y1 = max(0, (int)a->y1), x2 = min(W - 1, (int)a->x2), y2 = min(H - 1, (int)a->y2);
  if (x1 > x2 || y1 > y2) return;
  int aw = a->x2 - a->x1 + 1, w = x2 - x1 + 1;
  for (int y = y1; y <= y2; y++)
    memcpy(mirror + y * W + x1, px + ((y - a->y1) * aw + (x1 - a->x1)) * 2, w * 2);
  markDirty({(int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2});
}

static bool put(WiFiClient& c, const void* p, size_t n) { return c.write((const uint8_t*)p, n) == n; }

static bool sendRect(WiFiClient& c, const Rect& r) {
  int w = r.x2 - r.x1 + 1, h = r.y2 - r.y1 + 1;
  uint8_t hd[9] = {1, (uint8_t)r.x1, (uint8_t)(r.x1 >> 8), (uint8_t)r.y1, (uint8_t)(r.y1 >> 8),
                   (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8)};
  if (!put(c, hd, sizeof(hd))) return false;
  if (w == W) return put(c, mirror + r.y1 * W, (size_t)w * h * 2);  // full rows are contiguous
  for (int y = r.y1; y <= r.y2; y++)
    if (!put(c, mirror + y * W + r.x1, w * 2)) return false;
  return true;
}

static bool sendSound(WiFiClient& c) {
  SoundStage st = sound_stage();
  int v = st == SND_NONE ? 0 : constrain((int)(sound_db() * 10 + 0.5f), 0, 65535);
  uint8_t p[4] = {2, (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(int8_t)st};
  return put(c, p, sizeof(p));
}

static void streamTask(void*) {
  WiFiClient* c = nullptr;
  uint32_t lastSound = 0;
  for (;;) {
    WiFiClient* next;
    if (xQueueReceive(handoff, &next, c ? 0 : portMAX_DELAY) == pdTRUE) {
      if (c) {
        c->stop();
        delete c;
      }
      c = next;
      lastSound = 0;
    }
    if (!c) continue;
    vTaskDelay(pdMS_TO_TICKS(100));
    Rect r[MAX_RECTS];
    portENTER_CRITICAL(&mux);
    int n = dirtyN;
    memcpy(r, dirty, sizeof(Rect) * n);
    dirtyN = 0;
    portEXIT_CRITICAL(&mux);
    bool ok = c->connected();
    for (int i = 0; ok && i < n; i++) ok = sendRect(*c, r[i]);
    if (ok && millis() - lastSound >= 1000) {
      lastSound = millis();
      ok = sendSound(*c);
    }
    if (!ok) {
      LOG("stream: viewer left");
      if (!uxQueueMessagesWaiting(handoff)) live = false;  // a new viewer keeps it on
      c->stop();
      delete c;
      c = nullptr;
    }
  }
}

bool stream_start(WiFiClient& client) {
  lv_display_t* disp = lv_display_get_default();
  if (!mirror) {
    W = lv_display_get_horizontal_resolution(disp);
    H = lv_display_get_vertical_resolution(disp);
    mirror = (uint16_t*)heap_caps_calloc((size_t)W * H, 2, MALLOC_CAP_SPIRAM);
    if (!mirror) {
      LOG("stream: no PSRAM for the screen copy");
      return false;
    }
    handoff = xQueueCreate(1, sizeof(WiFiClient*));
    xTaskCreatePinnedToCore(streamTask, "stream", 3072, nullptr, 1, nullptr, 0);
  }
  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-store\r\n"
               "Connection: close\r\n\r\nFRDY");
  uint8_t size[4] = {(uint8_t)W, (uint8_t)(W >> 8), (uint8_t)H, (uint8_t)(H >> 8)};
  client.write(size, sizeof(size));
  WiFiClient* mine = new WiFiClient(client);
  portENTER_CRITICAL(&mux);
  dirtyN = 0;
  portEXIT_CRITICAL(&mux);
  if (xQueueSend(handoff, &mine, pdMS_TO_TICKS(500)) != pdTRUE) {
    delete mine;
    return true;  // headers are out; the browser retries
  }
  live = true;
  // One full redraw fills the mirror (and goes out as the first frame).
  lv_obj_invalidate(lv_screen_active());
  LOG("stream: viewer connected");
  return true;
}
