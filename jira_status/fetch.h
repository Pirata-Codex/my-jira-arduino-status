#pragma once
#include <Arduino.h>

// Data is fetched in one blocking pass while the "Refreshing" screen is shown (no background task):
// the dashboard is torn down first so the HTTPS handshakes get all the RAM.
enum FetchStep { STEP_CARDS, STEP_SPRINT, STEP_KPI, STEP_KUMA, STEP_CALENDAR, STEP_COUNT };
enum StepState { STEP_WAIT, STEP_RUN, STEP_OK, STEP_FAIL, STEP_SKIP };
typedef void (*FetchProgress)(FetchStep step, StepState state, const String& detail);

// What is due: nothing, only Uptime Kuma (short interval), or everything.
// REFRESH_STATUS: only your presence status (Jira user property), checked every minute in place.
enum RefreshKind { REFRESH_NONE, REFRESH_KUMA, REFRESH_FULL, REFRESH_STATUS };
RefreshKind fetch_due();
void fetch_all(FetchProgress progress, RefreshKind kind);
// Refresh on the next loop (after saving settings, or /refresh).
void fetch_request_refresh();
// Skip this minute's status check (too little memory); it runs again with the next refresh.
void fetch_status_postpone();
