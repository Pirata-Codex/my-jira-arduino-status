#include "led.h"
#include "config.h"
#include "settings.h"
#include "sound.h"

// A task on core 0 draws the grid 50 times a second, so the light keeps moving while loop() is busy
// with an HTTPS refresh. Everything it reads is set from other tasks as single values.

#if LED_ENABLED
#include <Adafruit_NeoPixel.h>

static const int N = 4;  // 4 x 4
static Adafruit_NeoPixel grid(N * N, LED_PIN, NEO_GRB + NEO_KHZ800);
static volatile uint8_t scene = LED_IDLE;
static volatile float soonLeft = 1;
static volatile uint8_t status = 0;
static volatile uint32_t notifyColor = 0, notifyAt = 0;  // notifyAt 0: no notification
static volatile uint16_t notifyShape = LED_FILL;

// Board LED number of a pixel as seen through the window (row 0 at the top). LED_ROTATE turns the
// picture in 90-degree steps for the way the board is mounted; LED_SERPENTINE for boards whose
// odd rows run backwards.
static int at(int r, int c) {
  for (int k = 0; k < LED_ROTATE; k++) {
    int t = r;
    r = c;
    c = N - 1 - t;
  }
  if (LED_SERPENTINE && (r & 1)) c = N - 1 - c;
  return r * N + c;
}

static void put(int r, int c, uint32_t col, float k) {
  k = constrain(k, 0.0f, 1.0f);
  grid.setPixelColor(at(r, c), (uint8_t)((col >> 16 & 0xFF) * k), (uint8_t)((col >> 8 & 0xFF) * k),
                     (uint8_t)((col & 0xFF) * k));
}

static void fill(uint32_t col, float k) {
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++) put(r, c, col, k);
}

static float breathe(float t, float period) { return 0.5f - 0.5f * cosf(t * 2 * PI / period); }

// Meeting soon: amber fills the grid pixel by pixel from the bottom-left as the start gets closer
// (~19 s per pixel over the last 5 minutes). The pixel being filled breathes, so it reads as "counting".
static void soon(float t) {
  float lit = (1 - soonLeft) * N * N;
  int n = 0;
  for (int r = N - 1; r >= 0; r--)
    for (int c = 0; c < N; c++, n++) {
      float k = n < (int)lit ? 0.8f : n == (int)lit ? 0.1f + 0.6f * breathe(t, 2) : 0;
      put(r, c, LED_AMBER, k);
    }
}

// No alert: dark, but for the room in the middle and the status lights in the corners.
static void idle(float t) {
  grid.clear();
  SoundStage st = sound_stage();
  if (st == SND_DISTRACTING || st == SND_TOO_LOUD) {
    bool loud = st == SND_TOO_LOUD;
    float k = loud ? 0.25f + 0.75f * breathe(t, 1) : 0.3f + 0.2f * breathe(t, 3);
    for (int r = 1; r < 3; r++)
      for (int c = 1; c < 3; c++) put(r, c, loud ? LED_RED : LED_AMBER, k);
  }
  uint8_t s = status;
  bool blink = fmodf(t, 1) < 0.5f;
  if (s & LED_ST_AWAY) put(0, 0, LED_VIOLET, 0.6f);
  if (s & LED_ST_MIC) put(0, N - 1, LED_RED, 0.8f);
  if (s & LED_ST_LOWBATT) put(N - 1, 0, LED_RED, blink ? 0.8f : 0);
  else if (s & LED_ST_FULL) put(N - 1, 0, LED_LIME, 0.6f);
  else if (s & LED_ST_CHARGING) put(N - 1, 0, LED_LIME, 0.1f + 0.6f * breathe(t, 3));
  if (s & LED_ST_NOWIFI) put(N - 1, N - 1, LED_CYAN, blink ? 0.7f : 0);
}

static void cross(float t) {
  bool on = fmodf(t, 1) < 0.6f;
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++) put(r, c, LED_RED, (LED_CROSS >> (r * N + c) & 1) ? (on ? 1 : 0.1f) : 0);
}

// Notification: the shape sweeps on row by row (0.3 s), holds, fades out by 1.4 s. False when there is none.
static bool notification() {
  uint32_t start = notifyAt;
  if (!start) return false;
  float s = (millis() - start) / 1000.0f;
  if (s >= 1.4f) {
    notifyAt = 0;
    return false;
  }
  float fade = s < 0.9f ? 1 : 1 - (s - 0.9f) / 0.5f;
  uint16_t shape = notifyShape;
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++)
      put(r, c, notifyColor, (shape >> (r * N + c) & 1) ? constrain((s - r * 0.08f) / 0.1f, 0.0f, 1.0f) * fade : 0);
  return true;
}

static void ledTask(void*) {
  bool dark = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t b = gCfg.ledBright;
    if (!b) {
      if (!dark) {
        grid.clear();
        grid.show();
        dark = true;
      }
      continue;
    }
    dark = false;
    grid.setBrightness(b);  // every pixel is drawn again below, so the scaling never compounds
    // Time wraps every 420 s, a whole number of cycles of every animation (no jump at the wrap).
    float t = (millis() % 420000) / 1000.0f;
    if (!notification()) {
      switch (scene) {
        case LED_SOON: soon(t); break;
        case LED_LIVE: fill(LED_RED, 0.15f + 0.85f * breathe(t, 1.4f)); break;
        case LED_DOWN: cross(t); break;
        default: idle(t);
      }
    }
    grid.show();
  }
}

void led_begin() {
  grid.begin();
  grid.show();
  // Core 0 with WiFi and the microphone, low priority; the screen (core 1) never waits for it.
  xTaskCreatePinnedToCore(ledTask, "led", 2560, nullptr, 1, nullptr, 0);
  led_notify(LED_ORANGE);
  LOG("led: 4x4 grid on GPIO %d, brightness %u", LED_PIN, gCfg.ledBright);
}

void led_scene(LedScene s, float left) {
  soonLeft = constrain(left, 0.0f, 1.0f);
  if (s == scene) return;
  static const uint32_t SWEEP[] = {0, LED_AMBER, LED_RED, LED_RED};
  if (s > scene && SWEEP[s]) led_notify(SWEEP[s], s == LED_DOWN ? LED_CROSS : LED_FILL);
  scene = s;
}

void led_status(uint8_t flags) { status = flags; }

void led_notify(uint32_t rgb, uint16_t shape) {
  notifyColor = rgb;
  notifyShape = shape;
  notifyAt = millis() | 1;  // never 0
}
#else
void led_begin() {}
void led_scene(LedScene, float) {}
void led_status(uint8_t) {}
void led_notify(uint32_t, uint16_t) {}
#endif
