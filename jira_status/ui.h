#pragma once
#include <Arduino.h>

void ui_init();
// Full-screen message while booting / waiting for WiFi.
void ui_boot(const String& title, const String& detail);
// Shows the "Refreshing" checklist, tears the dashboard down, fetches everything, then rebuilds and shows it.
void ui_refresh();
// Call from loop(): clock, page rotation, banners, redraw on new data.
void ui_tick();
void ui_apply_settings();
