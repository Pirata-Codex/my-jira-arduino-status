#include "sound.h"
#include "config.h"
#include "settings.h"

// The MAX4466 gives the sound wave around half the supply voltage. Every ~50 ms the board takes
// 200 samples (~4 kHz), removes the DC level and computes the RMS amplitude:
//   level (dB) = 20 * log10(RMS in ADC steps) + calibration
// The calibration (settings page) shifts it so that a quiet room reads about 35-40 dB. It's an
// approximation of dB(A), good enough to tell a quiet office from a loud one.
// The level is smoothed over ~5 s for the stage, so a door slam doesn't change it, and the stage
// only changes after moving 2 dB past a limit (no flicker at the border).

static const char* NAMES[] = {"FOCUS", "NORMAL", "DISTRACTING", "TOO LOUD"};
// Upper limits (dB) of FOCUS, NORMAL and DISTRACTING; above the last is TOO LOUD.
static const float LIMITS[] = {SOUND_FOCUS_MAX, SOUND_NORMAL_MAX, SOUND_DISTRACTING_MAX};
static const float HYSTERESIS = 2;

static volatile float levelDb = 0;
static volatile int8_t stage = SND_NONE;

const char* sound_name(SoundStage s) { return s >= 0 && s <= SND_TOO_LOUD ? NAMES[s] : ""; }
SoundStage sound_stage() { return (SoundStage)stage; }
float sound_db() { return levelDb; }

#if MIC_ENABLED
static int8_t stageFor(float db, int8_t current) {
  int8_t s = 0;
  while (s < 3 && db >= LIMITS[s]) s++;
  if (current < 0 || s == current) return s;
  // Hysteresis: only leave the current stage when clearly past its border.
  if (s > current && db < LIMITS[current] + HYSTERESIS) return current;
  if (s < current && db > LIMITS[current - 1] - HYSTERESIS) return current;
  return s;
}

static void micTask(void*) {
  static const int N = 200;
  static uint16_t buf[N];
  float smooth = 0;
  int windows = 0;
  for (;;) {
    uint32_t sum = 0;
    for (int i = 0; i < N; i++) {
      buf[i] = analogRead(MIC_PIN);
      sum += buf[i];
      delayMicroseconds(200);
    }
    float mean = (float)sum / N, acc = 0;
    for (int i = 0; i < N; i++) {
      float d = buf[i] - mean;
      acc += d * d;
    }
    float rms = sqrtf(acc / N);
    float db = 20 * log10f(rms < 1 ? 1 : rms) + gCfg.micCal;
    smooth = windows ? smooth + (db - smooth) * 0.012f : db;  // ~5 s
    if (windows < 1000) windows++;
    levelDb = smooth;
    if (windows >= 40) stage = stageFor(smooth, stage);  // ~2 s to settle after boot
    vTaskDelay(pdMS_TO_TICKS(10));  // let the idle task run (task watchdog)
  }
}

void sound_begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
  // Core 0, low priority: WiFi preempts it; the screen (core 1) never waits for it.
  xTaskCreatePinnedToCore(micTask, "mic", 3072, nullptr, 1, nullptr, 0);
  LOG("sound: microphone on GPIO %d, calibration %d dB", MIC_PIN, gCfg.micCal);
}
#else
void sound_begin() {}
#endif
