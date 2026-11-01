#include "led.h"
#include "config.h"
#include "settings.h"
#include "sound.h"

// A task on core 0 draws the ring 50 times a second, so the light keeps moving while loop() is busy
// with an HTTPS refresh. Everything it reads is set from other tasks as single values.

#if LED_ENABLED
#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
static volatile uint8_t scene = LED_IDLE;
static volatile float soonLeft = 1;
static volatile bool micDot = false;
static volatile uint32_t notifyColor = 0, notifyAt = 0;  // notifyAt 0: no notification

static void put(int i, uint32_t c, float k) {
  k = constrain(k, 0.0f, 1.0f);
  ring.setPixelColor(i, (uint8_t)((c >> 16 & 0xFF) * k), (uint8_t)((c >> 8 & 0xFF) * k), (uint8_t)((c & 0xFF) * k));
}

// Brightness of LED i under an arc whose head is at `head` (LEDs, fractional) with a tail `len` long.
static float arc(int i, float head, float len) {
  float d = fmodf(head - i + LED_COUNT, LED_COUNT);
  if (d > LED_COUNT - 1) return d - (LED_COUNT - 1);  // the LED the head is moving into
  return d < len ? 1 - d / len : 0;
}

static float breathe(float t, float period) { return 0.5f - 0.5f * cosf(t * 2 * PI / period); }

// The HUD's spinning arcs: a dim ring, a bright arc going around every 6 s and a fainter one opposite.
static void reactor(float t, uint32_t c) {
  float h = fmodf(t * LED_COUNT / 6, LED_COUNT);
  for (int i = 0; i < LED_COUNT; i++)
    put(i, c, 0.1f + 0.9f * arc(i, h, 5) + 0.4f * arc(i, fmodf(h + LED_COUNT / 2.0f, LED_COUNT), 3));
}

static void away(float t) {
  for (int i = 0; i < LED_COUNT; i++) put(i, LED_VIOLET, 0.08f + 0.5f * breathe(t, 5));
}

// Meeting soon: the lit part of the ring is the time left.
static void soon(float t) {
  float lit = soonLeft * LED_COUNT, k = 0.35f + 0.65f * breathe(t, 2);
  for (int i = 0; i < LED_COUNT; i++) put(i, LED_AMBER, max(0.04f, constrain(lit - i, 0.0f, 1.0f) * k));
}

static void live(float t) {
  for (int i = 0; i < LED_COUNT; i++) put(i, LED_RED, 0.15f + 0.85f * breathe(t, 1.4f));
}

static void alarmArcs(float t) {
  float h = fmodf(t * LED_COUNT / 1.2f, LED_COUNT);
  for (int i = 0; i < LED_COUNT; i++)
    put(i, LED_RED, 0.08f + arc(i, h, 4) + arc(i, fmodf(h + LED_COUNT / 2.0f, LED_COUNT), 4));
}

// Notification: wipe around the ring (0.5 s), hold (0.3 s), fade (0.6 s). False when there is none.
static bool notification() {
  uint32_t at = notifyAt;
  if (!at) return false;
  float s = (millis() - at) / 1000.0f;
  if (s >= 1.4f) {
    notifyAt = 0;
    return false;
  }
  float fill = s / 0.5f * LED_COUNT, fade = s < 0.8f ? 1 : 1 - (s - 0.8f) / 0.6f;
  for (int i = 0; i < LED_COUNT; i++) put(i, notifyColor, constrain(fill - i, 0.0f, 1.0f) * fade);
  return true;
}

static void ledTask(void*) {
  bool dark = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t b = gCfg.ledBright;
    if (!b) {
      if (!dark) {
        ring.clear();
        ring.show();
        dark = true;
      }
      continue;
    }
    dark = false;
    ring.setBrightness(b);  // every pixel is drawn again below, so the scaling never compounds
    // Time wraps every 420 s, a whole number of turns of every animation (no jump at the wrap).
    float t = (millis() % 420000) / 1000.0f;
    if (!notification()) {
      switch (scene) {
        case LED_AWAY: away(t); break;
        case LED_SOON: soon(t); break;
        case LED_LIVE: live(t); break;
        case LED_DOWN: alarmArcs(t); break;
        default: {
          SoundStage st = sound_stage();
          reactor(t, st == SND_TOO_LOUD ? LED_RED : st == SND_DISTRACTING ? LED_AMBER : LED_CYAN);
        }
      }
    }
    if (micDot) put(LED_MIC_INDEX, LED_RED, 1);
    ring.show();
  }
}

void led_begin() {
  ring.begin();
  ring.show();
  // Core 0 with WiFi and the microphone, low priority; the screen (core 1) never waits for it.
  xTaskCreatePinnedToCore(ledTask, "led", 2560, nullptr, 1, nullptr, 0);
  led_notify(LED_CYAN);
  LOG("led: %d LEDs on GPIO %d, brightness %u", LED_COUNT, LED_PIN, gCfg.ledBright);
}

void led_scene(LedScene s, float left) {
  soonLeft = constrain(left, 0.0f, 1.0f);
  if (s == scene) return;
  static const uint32_t SWEEP[] = {0, 0, LED_AMBER, LED_RED, LED_RED};
  if (s > scene && SWEEP[s]) led_notify(SWEEP[s]);
  scene = s;
}

void led_mic(bool on) { micDot = on; }

void led_notify(uint32_t rgb) {
  notifyColor = rgb;
  notifyAt = millis() | 1;  // never 0
}
#else
void led_begin() {}
void led_scene(LedScene, float) {}
void led_mic(bool) {}
void led_notify(uint32_t) {}
#endif
