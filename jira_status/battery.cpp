#include "battery.h"
#include "config.h"
#include "rfid.h"
#include "ui.h"
#include "led.h"

#if BATT_ENABLED
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>

static volatile int rawAvg = -1;
static adc_cali_handle_t cali = nullptr;
static float volts = 0;
static int percent = -1;
static uint32_t lastRead = 0;
static bool warned = false;

void battery_raw(int raw) { rawAvg = raw; }

// Li-ion resting voltage to charge left. Under the board's ~0.5 A the pack reads a little lower.
static int toPercent(float v) {
  static const float V[] = {3.30f, 3.40f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
  static const int P[] = {0, 5, 10, 20, 35, 50, 65, 80, 90, 100};
  if (v <= V[0]) return 0;
  for (int i = 1; i < 10; i++)
    if (v < V[i]) return P[i - 1] + (int)((v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]));
  return 100;
}

void battery_begin() {
  adc_unit_t unit;
  adc_channel_t ch;
  if (adc_oneshot_io_to_channel(BATT_PIN, &unit, &ch) != ESP_OK || unit != ADC_UNIT_1) {
    LOG("battery: GPIO %d is not an ADC1 pin, gauge off", BATT_PIN);
    return;
  }
#if MIC_ENABLED
  adc_cali_curve_fitting_config_t cc = {};
  cc.unit_id = unit;
  cc.chan = ch;
  cc.atten = ADC_ATTEN_DB_12;
  cc.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_cali_create_scheme_curve_fitting(&cc, &cali) != ESP_OK) cali = nullptr;
#endif
  LOG("battery: gauge on GPIO %d", BATT_PIN);
}

void battery_loop() {
  if (millis() - lastRead < 1000) return;
  lastRead = millis();
  int mv = -1;
#if MIC_ENABLED
  int raw = rawAvg;
  if (raw >= 0 && cali && adc_cali_raw_to_voltage(cali, raw, &mv) != ESP_OK) mv = -1;
#else
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BATT_PIN);
  mv = sum / 16;
#endif
  if (mv < 0) return;
  float v = mv * BATT_RATIO / 1000.0f;
  if (v < 2.5f) {  // nothing on the divider: running from USB without a pack
    percent = -1;
    volts = 0;
    return;
  }
  volts = volts > 0 ? volts + (v - volts) * 0.1f : v;  // ~10 s smoothing: the load pulses with WiFi
  percent = toPercent(volts);
  bool docked = rfid_present();
  if (percent <= 10 && !docked && !warned) {
    warned = true;
    LOG("battery: low, %.2f V", volts);
    ui_toast("BATTERY LOW, DOCK ME", 8000);
    led_notify(LED_RED, LED_BOTTOM);
  }
  if (percent > 20 || docked) warned = false;
}

int battery_percent() { return percent; }
float battery_volts() { return volts; }
#else
void battery_begin() {}
void battery_loop() {}
int battery_percent() { return -1; }
float battery_volts() { return 0; }
void battery_raw(int) {}
#endif
