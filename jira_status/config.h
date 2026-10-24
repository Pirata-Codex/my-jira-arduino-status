#pragma once

// Panel is 320x480 portrait natively; LVGL rotates it to 480x320 landscape.
#define TFT_HOR_RES 320
#define TFT_VER_RES 480
#define SCREEN_W 480
#define SCREEN_H 320
// 90 = landscape. Use LV_DISPLAY_ROTATION_270 if the picture is upside down.
#define SCREEN_ROTATION LV_DISPLAY_ROTATION_90

#define WIFI_SETUP_AP "Jira-Status-Setup"
#define HOSTNAME "jira-status"   // http://jira-status.local

// Iran Standard Time, no DST.
#define TZ_INFO "<+0330>-3:30"
// NTP servers, tried in order. The company network blocks outside NTP, so its domain controller
// comes first (it doesn't resolve elsewhere, so the board falls through to time.ir, then Google).
#define NTP_1 "OP-DC02-SRV.orchidpharmed.com"
#define NTP_2 "ntp.time.ir"
#define NTP_3 "time.google.com"
// Mail server address inside the company network. When the calendar link fails, the board tries
// the same path on this host over plain http ("" turns this off).
#define ICS_LAN_HOST "10.20.36.36"

// Memory guards (bytes of the largest free block). One HTTPS connection needs roughly 35-45 KB in
// one piece. Kuma checks run in place while at least KUMA_INPLACE_BLOCK is free; a refresh that
// still finds less than MIN_TLS_BLOCK after freeing the dashboard restarts the board instead.
#define KUMA_INPLACE_BLOCK 50000
#define MIN_TLS_BLOCK 38000
// Daily restart for a clean heap (local hour; -1 turns it off).
#define NIGHTLY_RESTART_HOUR 4

#define FETCH_JIRA_MS     (2 * 60 * 1000UL)
#define FETCH_KUMA_MS     (60 * 1000UL)
#define FETCH_CALENDAR_MS (5 * 60 * 1000UL)

#define MEETING_SOON_SEC   (5 * 60)   // slim banner before start
#define MEETING_ALERT_SEC  (2 * 60)   // pulsing banner until 2 min after start

// Serial log (115200 baud): timestamp, free heap and largest free block on every line.
#define LOG(fmt, ...) Serial.printf("[%7lu] heap %3uk blk %3uk | " fmt "\n", millis(), \
    (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024), ##__VA_ARGS__)
