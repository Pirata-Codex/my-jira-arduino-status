#pragma once
// (Not "stream.h": Windows file names ignore case, and that would hide the core's Stream.h.)
#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

// Live view of the screen (/stream in the browser), with the room sound level next to it and,
// on request, the room audio. Needs
// PSRAM (ESP32-S3): the board keeps a copy of the screen there while someone watches; without
// PSRAM /stream says so and nothing changes.
void stream_capture(const lv_area_t* area, const uint8_t* px);  // from the display flush
// Takes over the request's connection and streams to it (replaces the previous viewer); with
// audio, the room microphone too. False when there's no PSRAM for the copy.
bool stream_start(WiFiClient& c, bool audio);
// True while a viewer listens to the microphone (the board shows it in the header).
bool liveview_listening();
void liveview_audio(const int16_t* s, int n);  // from the microphone task
