#include "sound.h"
#include "config.h"
#include "settings.h"
#include "liveview.h"

// The MAX4466 gives the sound wave around half the supply voltage. The ADC samples it all the time
// (continuous mode: DMA, no CPU per sample) at 64 kHz; every 8 samples are averaged into one 8 kHz
// sample, which cuts the ADC's own hiss to about a third. That stream feeds two things:
//   - the level: every 50 ms the RMS amplitude around the DC level,
//       level (dB) = 20 * log10(RMS in ADC steps) + calibration
//     The calibration (settings page) shifts it so that a quiet room reads about 35-40 dB. It's an
//     approximation of dB(A), good enough to tell a quiet office from a loud one.
//   - the audio for the live view, only while someone listens (/stream, "Listen").
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
#include <esp_adc/adc_continuous.h>

// The classic ESP32 reports ADC results in the TYPE1 layout, the S3 in TYPE2.
#if CONFIG_IDF_TARGET_ESP32
#define MIC_FORMAT ADC_DIGI_OUTPUT_FORMAT_TYPE1
#define MIC_CHANNEL(p) ((p)->type1.channel)
#define MIC_VALUE(p) ((p)->type1.data)
#else
#define MIC_FORMAT ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define MIC_CHANNEL(p) ((p)->type2.channel)
#define MIC_VALUE(p) ((p)->type2.data)
#endif

static const int OVERSAMPLE = 8;           // ADC samples averaged into one audio sample
static const int ADC_HZ = MIC_AUDIO_HZ * OVERSAMPLE;  // 64 kHz (the S3 goes up to 83 kHz)
static const int BLOCK = MIC_AUDIO_HZ / 20;  // audio samples per level window (50 ms)

static adc_continuous_handle_t adc;
static adc_channel_t channel;

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
  static uint8_t raw[1024];
  static int16_t pcm[128];
  int pcmN = 0, pairN = 0, pairSum = 0, blockN = 0, windows = 0;
  float dc = 2048, acc = 0, smooth = 0, lp = 0;
  // Diagnostics, logged every 30 s: samples per second and the raw range (bias, clipping, wiring).
  uint32_t statAt = millis(), statN = 0;
  int rawMin = 4095, rawMax = 0;
  for (;;) {
    uint32_t got = 0;
    if (adc_continuous_read(adc, raw, sizeof(raw), &got, 1000) != ESP_OK) continue;  // blocks: lets others run
    bool listening = liveview_listening();
    for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got; i += SOC_ADC_DIGI_RESULT_BYTES) {
      auto* p = (adc_digi_output_data_t*)&raw[i];
      if (MIC_CHANNEL(p) != channel) continue;
      int v = MIC_VALUE(p);
      if (v < rawMin) rawMin = v;
      if (v > rawMax) rawMax = v;
      pairSum += v;
      if (++pairN < OVERSAMPLE) continue;
      float x = (float)pairSum / OVERSAMPLE;
      pairN = pairSum = 0;
      statN++;
      // Voice band: the bias tracker removes everything below ~80 Hz (bias, hum, desk thumps), a
      // light low-pass softens the hiss above ~3 kHz.
      dc += (x - dc) * 0.06f;
      float hp = x - dc;
      lp += (hp - lp) * 0.9f;
      float d = lp;
      acc += d * d;
      if (++blockN == BLOCK) {
        float rms = sqrtf(acc / BLOCK);
        float db = 20 * log10f(rms < 1 ? 1 : rms) + gCfg.micCal;
        smooth = windows ? smooth + (db - smooth) * 0.012f : db;  // ~5 s
        if (windows < 1000) windows++;
        levelDb = smooth;
        if (windows >= 40) stage = stageFor(smooth, stage);  // ~2 s to settle after boot
        acc = 0;
        blockN = 0;
      }
      if (listening) {
        // 12-bit wave to 16-bit audio (the gain screw on the board sets the loudness).
        pcm[pcmN++] = (int16_t)constrain((int)(d * 16), -32768, 32767);
        if (pcmN == 128) {
          liveview_audio(pcm, pcmN);
          pcmN = 0;
        }
      }
    }
    if (!listening) pcmN = 0;
    if (millis() - statAt >= 30000) {
      LOG("sound: %u samples/s, raw %d..%d, level %.0f dB", (unsigned)(statN * 1000 / (millis() - statAt)), rawMin,
          rawMax, (float)levelDb);
      statAt = millis();
      statN = 0;
      rawMin = 4095;
      rawMax = 0;
    }
  }
}

void sound_begin() {
  adc_unit_t unit;
  if (adc_continuous_io_to_channel(MIC_PIN, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) {
    LOG("sound: GPIO %d is not an ADC1 pin, microphone off", MIC_PIN);
    return;
  }
  adc_continuous_handle_cfg_t hc = {};
  hc.max_store_buf_size = 4096;
  hc.conv_frame_size = 1024;
  adc_digi_pattern_config_t pat = {};
  pat.atten = ADC_ATTEN_DB_12;
  pat.channel = channel;
  pat.unit = ADC_UNIT_1;
  pat.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
  adc_continuous_config_t cfg = {};
  cfg.pattern_num = 1;
  cfg.adc_pattern = &pat;
  cfg.sample_freq_hz = ADC_HZ;
  cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  cfg.format = MIC_FORMAT;
  if (adc_continuous_new_handle(&hc, &adc) != ESP_OK || adc_continuous_config(adc, &cfg) != ESP_OK ||
      adc_continuous_start(adc) != ESP_OK) {
    LOG("sound: ADC start failed, microphone off");
    return;
  }
  // Core 0, low priority: WiFi preempts it; the screen (core 1) never waits for it.
  xTaskCreatePinnedToCore(micTask, "mic", 3072, nullptr, 1, nullptr, 0);
  LOG("sound: microphone on GPIO %d, calibration %d dB", MIC_PIN, gCfg.micCal);
}
#else
void sound_begin() {}
#endif
