#pragma once
#include <esp_heap_caps.h>  // LOG

#define FRIDAY_VERSION "1.0.0"

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

// Presence status (Jira user property "friday.status" on your own account, written by your bot).
#define STATUS_PROPERTY "friday.status"
#define STATUS_POLL_MS (60 * 1000UL)
// The memory arena (lv_mem_pool.cpp): reserved before WiFi, shared by the screen and the HTTPS
// buffers, checked against the memory plan in memory.h.
#define LVGL_POOL_BYTES (72 * 1024)
// Badge reader (RC522, SPI) for arriving / leaving (rfid.cpp). Set RFID_ENABLED 0 without a reader.
// Pins for the ESP32-S3 board (the display uses 9-14 on its own SPI bus).
#define RFID_ENABLED 1
#define RFID_SCK 5
#define RFID_MOSI 6
#define RFID_MISO 7
#define RFID_SS 15
#define RFID_RST 16
#define RFID_POLL_MS 200     // how often the reader is asked for a card
#define RFID_REPEAT_MS 5000  // the same badge counts once per this many ms
// Room sound level (sound.cpp): MAX4466 microphone, OUT on an ADC1 pin (ADC2 doesn't work with WiFi).
#define MIC_ENABLED 1
#define MIC_PIN 1
#define MIC_AUDIO_HZ 8000  // live view audio ("Listen" on /stream): 8 kHz, 16-bit
// Stage limits in dB (approximate dB(A) after calibration): quiet office < 45, conversation ~55-60.
#define SOUND_FOCUS_MAX 45        // below: FOCUS (quiet enough for deep work)
#define SOUND_NORMAL_MAX 58       // below: NORMAL (usual office noise)
#define SOUND_DISTRACTING_MAX 70  // below: DISTRACTING; above: TOO LOUD
// Daily restart for a clean heap (local hour; -1 turns it off).
#define NIGHTLY_RESTART_HOUR 4

#define FETCH_JIRA_MS     (2 * 60 * 1000UL)
#define FETCH_KUMA_MS     (60 * 1000UL)
#define FETCH_CALENDAR_MS (5 * 60 * 1000UL)

#define MEETING_SOON_SEC   (5 * 60)   // slim banner before start
#define MEETING_ALERT_SEC  (2 * 60)   // pulsing banner until 2 min after start
// When today's meetings run out, the agenda shows the next ones from this many days ahead.
#define AGENDA_AHEAD_DAYS 7

// Serial log (115200 baud): timestamp, free heap and largest free block on every line. Byte-addressable
// RAM only: ESP.getFreeHeap() also counts ~36 KB of IRAM that neither LVGL nor TLS can use.
#define LOG(fmt, ...) Serial.printf("[%7lu] heap %3uk blk %3uk | " fmt "\n", millis(), \
    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024), \
    (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024), ##__VA_ARGS__)
