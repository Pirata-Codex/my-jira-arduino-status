#pragma once
#include <Arduino.h>

// WS2812B ring next to the screen (LED_PIN, LED_COUNT): the HUD's alerts as light. The UI sets the
// scene once a second; badge taps play a short notification over it.
//   IDLE  cyan arcs going around (amber / red while the room is DISTRACTING / TOO LOUD)
//   AWAY  violet, breathing (a presence status is set)
//   SOON  amber arc that shrinks as the meeting gets closer
//   LIVE  red pulse (meeting started)
//   DOWN  red alarm arcs (a service is down)
// Brightness: settings page (0 = off).
enum LedScene : uint8_t { LED_IDLE, LED_AWAY, LED_SOON, LED_LIVE, LED_DOWN };  // least to most urgent

// Colors tuned for the LEDs: the screen's hex values (theme.h) look washed out on WS2812s.
#define LED_CYAN   0x00B4FF
#define LED_LIME   0x60FF00
#define LED_VIOLET 0x7030FF
#define LED_AMBER  0xFF6A00
#define LED_RED    0xFF0A14

void led_begin();
// left: LED_SOON only, the share of the countdown still to go (1 = just started, 0 = starting now).
// A scene more urgent than the last one starts with a sweep in its color.
void led_scene(LedScene s, float left = 0);
void led_mic(bool on);          // one red LED while someone listens on /stream
void led_notify(uint32_t rgb);  // a sweep around the ring in this color (0xRRGGBB)
