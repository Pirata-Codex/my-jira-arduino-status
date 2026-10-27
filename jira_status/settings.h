#pragma once
#include <Arduino.h>

// Stored in NVS on the board (never in the repo). Edited on the board's web page.
struct Settings {
  String jiraUrl;     // e.g. https://jira.example.com
  // Backup addresses (optional), e.g. a relay at home that reaches the company network over VPN.
  // A fetch that fails on one address tries the other; the one that worked is tried first next time.
  String jiraUrl2, kumaUrl2, icsUrl2;
  String jiraPat;     // Jira personal access token (Bearer). Use a read-only account if you can.
  String jql;         // your sprint cards
  String spField;     // story points custom field, e.g. customfield_10006 (empty = count cards)
  String boardId;     // agile board id, for sprint name / end date (optional)
  String kpi1Label, kpi1Jql;
  String kpi2Label, kpi2Jql;
  String kumaUrl, kumaSlug;
  String icsUrl;      // published calendar (.ics) for meetings (optional)
  String webPass;     // settings page / OTA password (user "admin")
  uint8_t brightness; // 0..255
  uint16_t rotateSec; // seconds per screen
  uint16_t refreshMin; // minutes between full data refreshes (Jira, calendar, Kuma)
  uint16_t kumaMin;    // minutes between Uptime Kuma-only refreshes
  uint8_t cardsPerPage; // cards slide: rows per page (1..5)
  uint8_t cardPageSec;  // cards slide: seconds per page
  bool hideDone;        // cards slide: leave out Done cards
  String ownerName;     // F.R.I.D.A.Y. greets you by this name
  int8_t micCal;        // microphone calibration (dB added to the measured level)
  String rfidUids;      // your badge ids (hex, comma separated): tapping one sets IN / LEFT
  // Company WiFi, WPA2/WPA3-Enterprise with PEAP. Tried before the normal (WiFiManager) WiFi.
  String entSsid, entIdentity, entUser, entPass;
  String entCa;        // optional CA certificate (PEM) to verify the RADIUS server
};

extern Settings gCfg;
void settings_load();
void settings_save();
bool settings_mock();  // true while Jira isn't configured
