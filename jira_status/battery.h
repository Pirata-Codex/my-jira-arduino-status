#pragma once
#include <Arduino.h>

// Battery gauge: the pack voltage through a divider (BATT_PIN). The microphone keeps ADC1 busy in
// continuous mode, so sound.cpp samples this pin too and hands over the average (battery_raw).
void battery_begin();
void battery_loop();         // call from loop(): updates once a second, warns once when it runs low
int battery_percent();       // 0..100, -1 = no battery measured (USB power, or BATT_ENABLED 0)
float battery_volts();       // pack voltage, smoothed
void battery_raw(int raw);   // sound.cpp: average 12-bit reading of BATT_PIN
