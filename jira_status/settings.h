#pragma once
#include <Arduino.h>

// Stored in NVS on the board (never in the repo). Edited on the board's web page.
struct Settings {
  String jiraUrl;     // e.g. https://jira.example.com
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
  uint16_t refreshMin; // minutes between data refreshes
  // Company WiFi, WPA2/WPA3-Enterprise with PEAP. Tried before the normal (WiFiManager) WiFi.
  String entSsid, entIdentity, entUser, entPass;
  String entCa;        // optional CA certificate (PEM) to verify the RADIUS server
};

extern Settings gCfg;
void settings_load();
void settings_save();
bool settings_mock();  // true while Jira isn't configured
