#include "settings.h"
#include <Preferences.h>

Settings gCfg;
static Preferences prefs;

void settings_load() {
  prefs.begin("dash", true);
  gCfg.jiraUrl = prefs.getString("jiraUrl", "");
  gCfg.jiraPat = prefs.getString("jiraPat", "");
  gCfg.jql = prefs.getString("jql", "assignee = currentUser() AND sprint in openSprints() ORDER BY rank ASC");
  gCfg.spField = prefs.getString("spField", "");
  gCfg.boardId = prefs.getString("boardId", "");
  gCfg.kpi1Label = prefs.getString("k1Label", "ITSM open");
  gCfg.kpi1Jql = prefs.getString("k1Jql", "project = ITSM AND reporter = currentUser() AND resolution = Unresolved");
  gCfg.kpi2Label = prefs.getString("k2Label", "Overdue");
  gCfg.kpi2Jql = prefs.getString("k2Jql", "assignee = currentUser() AND resolution = Unresolved AND duedate < now()");
  gCfg.kumaUrl = prefs.getString("kumaUrl", "https://kuma.orchidpharmed.com");
  gCfg.kumaSlug = prefs.getString("kumaSlug", "crm");
  gCfg.icsUrl = prefs.getString("icsUrl", "");
  // Default password = last 6 hex digits of the chip id; shown on the boot screen.
  char def[8];
  snprintf(def, sizeof(def), "%06llx", ESP.getEfuseMac() & 0xFFFFFF);
  gCfg.webPass = prefs.getString("webPass", def);
  gCfg.brightness = prefs.getUChar("bright", 200);
  gCfg.rotateSec = prefs.getUShort("rotate", 12);
  prefs.end();
}

void settings_save() {
  prefs.begin("dash", false);
  prefs.putString("jiraUrl", gCfg.jiraUrl);
  prefs.putString("jiraPat", gCfg.jiraPat);
  prefs.putString("jql", gCfg.jql);
  prefs.putString("spField", gCfg.spField);
  prefs.putString("boardId", gCfg.boardId);
  prefs.putString("k1Label", gCfg.kpi1Label);
  prefs.putString("k1Jql", gCfg.kpi1Jql);
  prefs.putString("k2Label", gCfg.kpi2Label);
  prefs.putString("k2Jql", gCfg.kpi2Jql);
  prefs.putString("kumaUrl", gCfg.kumaUrl);
  prefs.putString("kumaSlug", gCfg.kumaSlug);
  prefs.putString("icsUrl", gCfg.icsUrl);
  prefs.putString("webPass", gCfg.webPass);
  prefs.putUChar("bright", gCfg.brightness);
  prefs.putUShort("rotate", gCfg.rotateSec);
  prefs.end();
}

bool settings_mock() { return gCfg.jiraUrl.isEmpty() || gCfg.jiraPat.isEmpty(); }
