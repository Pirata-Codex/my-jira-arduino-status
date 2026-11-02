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
static volatile bool micDot = false;
static volatile uint32_t notifyColor = 0, notifyAt = 0;  // notifyAt 0: no notification
static volatile uint16_t notifyShape = LED_FILL;

// The edge, clockwise from the top-left corner: the idle dot's path.
static const uint8_t EDGE[12][2] = {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 3}, {2, 3},
                                    {3, 3}, {3, 2}, {3, 1}, {3, 0}, {2, 0}, {1, 0}};

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

// One dot going around the edge every 4 s with a fading tail; the middle four glow faintly.
static void orbit(float t, uint32_t col) {
  float head = fmodf(t * 3, 12);
  for (int i = 0; i < 12; i++) {
    float d = fmodf(head - i + 12, 12), k = 0.03f;
    if (d > 11) k = d - 11;               // the pixel the dot is moving into
    else if (d < 4) k = max(k, 1 - d / 4);  // the dot and its tail
    put(EDGE[i][0], EDGE[i][1], col, k);
  }
  for (int r = 1; r < 3; r++)
    for (int c = 1; c < 3; c++) put(r, c, col, 0.05f);
}

// Meeting soon: the lit rows (from the bottom) are the time left.
static void soon(float t) {
  float lit = soonLeft * N, k = 0.35f + 0.65f * breathe(t, 2);
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++) put(r, c, LED_AMBER, max(0.03f, constrain(lit - (N - 1 - r), 0.0f, 1.0f) * k));
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
        case LED_AWAY: fill(LED_VIOLET, 0.08f + 0.5f * breathe(t, 5)); break;
        case LED_SOON: soon(t); break;
        case LED_LIVE: fill(LED_RED, 0.15f + 0.85f * breathe(t, 1.4f)); break;
        case LED_DOWN: cross(t); break;
        default: {
          SoundStage st = sound_stage();
          orbit(t, st == SND_TOO_LOUD ? LED_RED : st == SND_DISTRACTING ? LED_AMBER : LED_ORANGE);
        }
      }
    }
    if (micDot) put(N - 1, 0, LED_RED, 1);
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
  static const uint32_t SWEEP[] = {0, 0, LED_AMBER, LED_RED, LED_RED};
  if (s > scene && SWEEP[s]) led_notify(SWEEP[s], s == LED_DOWN ? LED_CROSS : LED_FILL);
  scene = s;
}

void led_mic(bool on) { micDot = on; }

void led_notify(uint32_t rgb, uint16_t shape) {
  notifyColor = rgb;
  notifyShape = shape;
  notifyAt = millis() | 1;  // never 0
}
#else
void led_begin() {}
void led_scene(LedScene, float) {}
void led_mic(bool) {}
void led_notify(uint32_t, uint16_t) {}
#endif
