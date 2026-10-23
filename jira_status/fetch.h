#pragma once

// Starts the background task (core 0) that polls Jira, Uptime Kuma and the calendar.
void fetch_begin();
// Asks the task to refresh everything now (after saving settings).
void fetch_request_refresh();
