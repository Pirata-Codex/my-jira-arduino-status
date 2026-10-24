<p align="center">
  <img src="docs/banner.svg" alt="F.R.I.D.A.Y. — a desk HUD for Jira, your calendar and service health" width="100%">
</p>

<p align="center">
  <img alt="version" src="https://img.shields.io/badge/F.R.I.D.A.Y.-v1.0.0-2fd6f0?style=for-the-badge&labelColor=03141a">
  <img alt="ESP32" src="https://img.shields.io/badge/ESP32-WROOM--32-a6e22e?style=for-the-badge&labelColor=03141a">
  <img alt="LVGL" src="https://img.shields.io/badge/LVGL-9.2-2fd6f0?style=for-the-badge&labelColor=03141a">
  <img alt="display" src="https://img.shields.io/badge/ILI9488-480×320-a6e22e?style=for-the-badge&labelColor=03141a">
</p>

<p align="center"><code>&gt; BOOT SEQUENCE … WIFI LINKED … JIRA SYNCED … CALENDAR SYNCED … KUMA ONLINE … READY, BOSS_</code></p>

**F.R.I.D.A.Y.** is a standalone desk display: an ESP32 and a 3.5" TFT showing your Jira sprint, your cards and KPIs, today's meetings, and the health of your services from Uptime Kuma, in an Iron Man style heads-up display. It joins WiFi and talks to Jira, Exchange and Kuma directly. No PC, no server.

## ◢ Screens

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/pulse.png" alt="PULSE screen"><br><b>PULSE</b> — clock, Jalali and Gregorian date, today's meeting timeline, sprint ring, and the cards you're working on.</td>
    <td width="50%"><img src="docs/screenshots/agenda.png" alt="AGENDA screen"><br><b>AGENDA</b> — the next meetings with room or Meet link, a LIVE tag on the one running and a countdown on the rest.</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/sprint.png" alt="SPRINT screen"><br><b>SPRINT</b> — points-done gauge, cards per status, and a burndown (actual vs ideal).</td>
    <td><img src="docs/screenshots/kpi.png" alt="KPI screen"><br><b>KPI</b> — points done, scope, WIP, blocked, and two tiles you define with JQL.</td>
  </tr>
</table>

Also: **CARDS** (every sprint card, tinted by status, paged) and **HEALTH** (one tile per Kuma monitor with ping and 24 h uptime). Screens rotate on their own; the timing is on the settings page.

> Screenshots come straight from the board: `http://jira-status.local/screenshot`.

### ◢ Alerts

A banner slides over every screen:

| Signal | When | Shows |
|---|---|---|
| 🔔 `NEXT MEETING` · amber | 5 min before a meeting | title, room / link, countdown |
| ▶ `MEETING LIVE` · pulsing | first 2 min of a meeting | title, room / link, time since start |
| ⚠ `SERVICE DOWN` · pulsing | a Kuma monitor is down | names, down / total |

Several at once rotate every 4 s with a `1/2` counter. The header line changes with them: `GOOD MORNING, BOSS`, `YOU'RE LIVE, BOSS`, `1 SERVICE(S) DOWN, BOSS`.

## ◢ Systems

```
┌─ JIRA ───────────┐  ┌─ CALENDAR ───────────┐  ┌─ KUMA ───────────┐
│ sprint, cards,   │  │ Exchange ICS link    │  │ public status    │
│ burndown, JQL    │  │ repeating meetings   │  │ page, ping and   │
│ KPIs (PAT auth)  │  │ (RRULE / EXDATE /    │  │ 24 h uptime      │
│                  │  │ moved / cancelled)   │  │ every minute     │
└────────┬─────────┘  └──────────┬───────────┘  └────────┬─────────┘
         └───────────── HTTPS, every 15 min ─────────────┘
                                 │
                   ┌─────────────▼─────────────┐
                   │  ESP32 · LVGL 9 · 480×320 │
                   └───────────────────────────┘
```

- **Calendar:** reads the whole published `.ics`, expands repeating meetings for today (daily / weekly, interval, by-day, until, count), drops deleted occurrences and applies moved or cancelled ones. On the office network it falls back to the internal mail server when the public address is refused. It refreshes every 15 minutes, on boot and at midnight.
- **Meeting place:** a room code like `5M3` or `BM4` from the location, title or description; otherwise a Google Meet or Skyroom link; otherwise the location text.
- **Network:** saved WiFi first, then company WiFi (WPA2-Enterprise / PEAP), then its own setup hotspot. Time comes from the company domain controller, then public NTP.
- **Memory:** the screen library has its own 48 KB region reserved at boot, so HTTPS keeps the large free block it needs. A refresh that still runs out of memory restarts the board cleanly, and it restarts itself every night at 04:00. The serial log prints free heap and largest block on every line.

## ◢ Hardware

| Part | Notes |
|---|---|
| ESP32 dev board | ESP32-WROOM-32, 4 MB flash. No PSRAM needed (a WROVER with PSRAM gives more headroom). |
| 3.5" SPI TFT, 320×480 | ILI9488 / ST7796 via TFT_eSPI. Pins: `SDA` `SCL` `CS` `DC` `RES` `BLK`. No MISO needed. |

## ◢ Build

1. **Board:** ESP32 Arduino core 3.x → **ESP32 Dev Module**.
2. **Partition scheme:** **Minimal SPIFFS (1.9MB APP with OTA)**.
3. **Libraries:** `lvgl` **9.2.x**, `TFT_eSPI`, `WiFiManager` (tzapu), `ArduinoJson` **7.x**.
4. **LVGL config:** copy `extras/lv_conf.h` to `Documents/Arduino/libraries/lv_conf.h`, next to the `lvgl` folder. Copy it again after updates; an old copy stops the build with a clear message.
5. **Screen:** set your panel and pins in TFT_eSPI's `User_Setup.h`.
6. **Upload** `jira_status/jira_status.ino`.

USB is needed **once**. After that, update over WiFi:

- **Browser:** *Sketch → Export compiled binary*, then upload `jira_status.ino.bin` as **Firmware** at `http://jira-status.local/update`.
- **Arduino IDE:** pick the network port `jira-status`.

## ◢ First run

1. Join the WiFi **Jira-Status-Setup** that the screen shows and pick your network.
2. The screen shows the settings URL and password. User `admin`; the default password is the last 6 hex digits of the chip ID.
3. Until Jira is set up the board shows demo data.
4. On the settings page fill in:
   - **Jira URL** and **Personal access token** (Jira profile → Personal Access Tokens, read access is enough).
   - **Story points field**, e.g. `customfield_10006`.
   - **Board ID** from the board URL (`rapidView=…`), for the sprint name and end date.
   - **Calendar .ics URL**: Outlook on the web → Settings → Calendar → Shared calendars → Publish a calendar → ICS link.
   - **Company WiFi** (optional): SSID, user and password for PEAP.

## ◢ Web console

| Path | Does |
|---|---|
| `/` | settings |
| `/refresh` | fetch everything now |
| `/screenshot` | the screen as a 480×320 BMP (redrawn once and streamed, no extra RAM) |
| `/update` | firmware upload |

All pages use the `admin` login.

## ◢ Secrets

The token and passwords live **only in the board's flash (NVS)**. They are never in this repo, and the settings page never sends the token back to the browser. The published calendar link is private too: anyone who has it can read your calendar.

## ◢ Known limits

- **Recurring meetings:** daily and weekly rules are expanded; monthly and yearly ones are skipped.
- **Persian text:** card titles use Vazirmatn (14, 16 and 20 px) with Montserrat for icons. The big numbers stay Latin only. To change sizes or ranges, edit and run `extras/make_fonts.sh` (needs Node).
- **Certificates:** HTTPS certificates aren't checked (`setInsecure`), because internal services often use a private CA.
- **Memory:** a 4 MB ESP32 without PSRAM is tight for HTTPS plus a full-screen UI. The board recovers by restarting when it runs out; a WROVER removes the limit.

<p align="center"><sub>F.R.I.D.A.Y. 1.0.0 · <code>[ SYSTEMS ONLINE ]</code></sub></p>
