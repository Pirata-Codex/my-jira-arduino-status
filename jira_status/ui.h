#pragma once
#include <Arduino.h>

void ui_init();
// Full-screen message while booting / waiting for WiFi.
void ui_boot(const String& title, const String& detail);
void ui_start_dashboard();
// Call from loop(): clock, page rotation, banners, redraw on new data.
void ui_tick();
void ui_apply_settings();
