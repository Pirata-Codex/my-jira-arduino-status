# Jira Status board

A desk display for an ESP32 and a 320×480 TFT, mounted landscape. It shows your Jira sprint, your cards and KPIs, today's meetings, and the health of the CRM services from Uptime Kuma.

The board works on its own: it joins WiFi and calls Jira, Kuma and your calendar directly. You don't need a PC or the Telegram bot.

## Screens

The screens rotate every few seconds. You can change the timing on the settings page.

| Screen | Shows |
|---|---|
| **Now** | A big clock and the date. The next meeting with a countdown. The card you're working on. |
| **Sprint** | A points-done gauge, days left, a card count per status, and a burndown line. The board records one point per day. |
| **My cards** | Your sprint cards, active ones first, each with a colored status pill and story points. |
| **KPIs** | Points done, sprint scope %, WIP, blocked cards, plus two KPI tiles you define with JQL. By default these are open ITSM tickets you raised and your overdue cards. |
| **Health** | One tile per Uptime Kuma monitor, with up/down, ping and 24h uptime. |

A banner appears over every screen:

- **Amber:** a meeting starts within 5 minutes.
- **Red, pulsing:** a meeting has started. It stays for the first 2 minutes.
- **Red, pulsing:** a Kuma monitor is down. It stays until the monitor is back up.

## Hardware

- An ESP32 dev board (ESP32-WROOM-32, 4 MB flash). No PSRAM is needed.
- A 320×480 SPI TFT (ILI9488 / ST7796) driven by TFT_eSPI. Reuse the `User_Setup.h` from the old `my-dashboard-arduino` sketch. It holds the panel driver and pins.

## Build (Arduino IDE)

1. **Board:** ESP32 Arduino core 3.x, then board **ESP32 Dev Module**.
2. **Partition scheme:** **Minimal SPIFFS (1.9MB APP with OTA)**. OTA needs two app slots.
3. **Libraries:**
   - `lvgl` **9.2.x**
   - `TFT_eSPI`
   - `WiFiManager` (tzapu)
   - `ArduinoJson` **7.x**
4. **LVGL config:** copy `extras/lv_conf.h` to `Documents/Arduino/libraries/lv_conf.h`, next to the `lvgl` folder.
5. **Screen setup:** set up TFT_eSPI's `User_Setup.h` for your panel, the same as for the old sketch.
6. **Upload:** open `jira_status/jira_status.ino` and upload.

You only need USB **once**. After that, update over WiFi in either of these ways:

- **Browser:** in the Arduino IDE use *Sketch → Export compiled binary*, then upload the `.bin` at `http://jira-status.local/update` (or the board's IP address).
- **Arduino IDE:** the board shows up as the network port `jira-status`.

## First run

1. The screen asks you to join the WiFi network **Jira-Status-Setup**. Pick your office network there.
2. The screen then shows the settings URL and password. The user is `admin`. The default password is the last 6 hex digits of the chip ID.
3. Until Jira is set up, the board shows **demo data**. Kuma is live from the start: it uses the public status page `kuma.orchidpharmed.com/status/crm`.
4. On the settings page, fill in these fields:
   - **Jira URL** and **Personal access token**. Create the token in your Jira profile under Personal Access Tokens. A token with read access is enough.
   - **Story points field**, for example `customfield_10006`. The Telegram bot's `Jira:FieldMap:story_points` has the right one.
   - **Board ID**, from the board URL `rapidView=…`. It gives the sprint name and end date.
   - **Calendar .ics URL** for meetings. In Outlook on the web it's under Settings → Calendar → Shared calendars → Publish a calendar → ICS link.

## Secrets

The token and passwords are kept **only in the board's NVS** (flash). They are never stored in this repo, and the web page never sends the token back to the browser.

## Known limits

- **Recurring meetings:** the ICS reader takes only the events of today. It doesn't expand recurring meetings (RRULE), so a weekly stand-up shows only on the day it was created. If your meetings repeat, the next step is a Microsoft Graph or Exchange calendar call.
- **Persian text:** the built-in Montserrat fonts have no Persian glyphs, so Persian card titles show as empty boxes. Persian shaping is already on in `lv_conf.h`. The fix is to generate a Vazirmatn font with `lv_font_conv` and set it as the fallback font.
- **Certificates:** HTTPS certificates aren't checked (`setInsecure`), because internal services often use a private CA.
