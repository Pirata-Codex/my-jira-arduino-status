#pragma once
#include <Arduino.h>

// 4x4 WS2812B grid (WCMCU-2812B-16) behind the frosted light window. Dark while all is fine, so any
// light means "look at the screen". The UI sets the scene once a second:
//   IDLE  off
//   SOON  amber filling up pixel by pixel from the bottom; full = the meeting starts now
//   LIVE  every pixel pulses red (meeting started)
//   DOWN  a red X blinks (a service is down)
// Short flashes: badge tap (lime check = welcome back, violet = see you, amber X = unknown badge),
// low battery (red bottom row). Brightness: settings page (0 = off).
enum LedScene : uint8_t { LED_IDLE, LED_SOON, LED_LIVE, LED_DOWN };  // least to most urgent

// Colors tuned for the LEDs: the screen's hex values (theme.h) look washed out on WS2812s.
#define LED_ORANGE 0xFF4A00
#define LED_LIME   0x60FF00
#define LED_VIOLET 0x7030FF
#define LED_AMBER  0xFF8A00
#define LED_RED    0xFF0A14

// Notification shapes: one bit per pixel, bit (row * 4 + column), row 0 at the top.
#define LED_FILL  0xFFFF
#define LED_CHECK ((1 << 7) | (1 << 8) | (1 << 10) | (1 << 13))
#define LED_CROSS ((1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 9) | (1 << 10) | (1 << 12) | (1 << 15))
#define LED_BOTTOM 0xF000  // the bottom row

void led_begin();
// left: LED_SOON only, the share of the countdown still to go (1 = just started, 0 = starting now).
// A scene more urgent than the last one starts with a sweep in its color.
void led_scene(LedScene s, float left = 0);
// A short flash (~1.4 s): `shape` sweeps on row by row in this color (0xRRGGBB), holds, fades.
void led_notify(uint32_t rgb, uint16_t shape = LED_FILL);
