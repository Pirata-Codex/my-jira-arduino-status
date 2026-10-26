# Memory budget

The board is an ESP32-WROOM without PSRAM. Every feature competes for the same few hundred KB of
RAM. This document explains how memory is laid out, planned, measured and guarded, and what to do
when you add a feature.

## What the board actually has (measured 2026-10-26)

| | |
|---|---|
| Byte-addressable RAM free at boot | ~183 KB, largest piece 107 KB |
| After WiFi (company login, PEAP) | ~71 KB free with a 56 KB pool, **largest piece 33 KB** |
| One HTTPS connection | ~53-60 KB at its peak, including **two buffers of ~17 KB, each in one piece** |
| Screen: always there | ~22 KB (header, tab bar, banner, LVGL itself) |
| Screen: a page | 6-16 KB (PULSE is the biggest); status screen 12 KB |

Two facts drive the whole design:

- **The WiFi login breaks the heap into pieces.** Before WiFi the biggest piece is 107 KB; after the
  company WiFi login it is 33 KB. TLS needs two separate 17 KB pieces, so a 33 KB piece is not
  enough, however much memory is free in total.
- **`ESP.getFreeHeap()` overstates.** It counts ~36 KB of IRAM that can only be used in 4-byte
  words. Neither TLS nor LVGL can use it. The log and `/memory` show byte-addressable memory only.

## Layout

```
boot ──► ARENA (72 KB, reserved before WiFi, while memory is still whole)
         [ chunk 0: 32 KB ][ chunk 1: 20 KB ][ chunk 2: 20 KB ]
           screen (LVGL) fills from the left ->   <- HTTPS (mbedTLS) fills from the right

         HEAP (~55 KB after WiFi): JSON documents, data text, sockets, web server,
         and whatever doesn't fit the arena
```

- During a refresh the dashboard is freed, the screen fits in chunk 0, and each TLS buffer gets a
  20 KB chunk of its own. That's why fetches run with the dashboard freed.
- With the dashboard on screen, a page may reach into chunk 1; a fetch then needs the heap's pieces
  for its buffers, and the admission check decides whether it can run.
- mbedTLS is pointed at the arena with `mbedtls_platform_set_calloc_free` after WiFi is up (the
  company WiFi login's own long-lived TLS state stays on the heap).

## The three mechanisms (`jira_status/memory.h`)

**1. The plan, checked at compile time.** `MEM_BUDGET` lists every feature with an estimate.
`static_assert`s check:

```
fetch:  every fetch estimate            <= TLS_EST (42 KB, arena) + FETCH_HEAP_MIN (24 KB, heap)
heap:   services + 2 x data text + fresh data + FETCH_HEAP_MIN           <= HEAP_BASE_EST
arena during a refresh:  base UI + refresh screen + TLS_EST              <= arena
arena chunks:  base UI + refresh screen fit chunk 0, a TLS buffer fits chunks 1 and 2
arena with the dashboard:  base UI + 2 x biggest page (slide)            <= arena
arena with a status:  base UI + status screen + health page              <= arena
```

A change that breaks the plan stops the build with `Memory plan: ...`.

**2. Measurement at run time.** Every time a feature runs, the board measures it:

- a fetch: free memory (heap + arena) before, minus the lowest point while it runs (sampled every
  4 ms), and whether it ran out of memory;
- a page or the status screen: LVGL's memory after building it minus before.

It logs `mem: fetch jira used 52k (estimate 66k, learned peak 52k)` and keeps the peaks in flash
(written only when they change notably).

**3. Admission before running.** `mem_admit(feature)`:

- **needs** = the measured peak once it has 3 measurements (the larger of estimate and measurement
  before that), plus 15% and 2 KB;
- a fetch also needs **two free pieces of 17 KB** (arena chunks or the heap's largest piece) and
  `FETCH_HEAP_MIN` on the heap.

| Feature | When there's no room |
|---|---|
| status check (every minute, dashboard on screen) | skipped; it runs with the next refresh |
| Kuma check | the dashboard is freed for it, as in a full refresh |
| full refresh, after freeing the dashboard | the board restarts (except on the first refresh after boot) |
| next page | the current page (or the status screen) is freed first; no slide |
| status screen | the dashboard page is freed first |

The refresh screen and the boot screen exist only while shown.

## Adding a feature

1. **Estimate.** Add a `MemFeature` and a line to `MEM_BUDGET`. A fetch is about 42 KB of TLS plus
   its JSON (ArduinoJson: about 1.2 x the filtered JSON text) on the heap. A screen is about
   120-200 bytes per LVGL object plus its text; count the objects.
2. **Check the plan.** Compile: `Memory plan: ...` means it doesn't fit. Make it cheaper, build it
   only while shown, or decide what to give up.
3. **Measure.** Wrap it in `mem_begin(f)` / `mem_end(f, ok, err)`, and call `mem_admit(f)` first
   wherever there's a fallback (postpone, free something, skip).
4. **Verify on the board.** Open `http://jira-status.local/memory` after it has run a few times. If
   the measured peak is far from the estimate, correct the estimate so the plan stays honest.

## Reading `/memory`

- **After WiFi** should be close to `HEAP_BASE_EST`. If it drops (a library update, a new global),
  update the constant and the plan re-checks.
- **TLS buffer slots** below 2 with the dashboard on screen is normal; fetches run during refreshes.
- **Measured peak** above the estimate: raise the estimate.
- **Out of memory** above 0: that feature failed at least once for lack of memory.
- **Forget measurements** clears the learned values, for example after a big change.

## Easy to miss

- A style change from a timer redraws the object even when the value didn't change; a large
  redraw 25 times a second starves everything else.
- `lv_anim` on a big object (a 150 px ring) redraws all of it every frame. Move small objects.
- Every `String` in a long-lived struct is a separate heap block. Keep `MAX_CARDS`,
  `MAX_MEETINGS`, ... as small as the screen needs.
- Keeping screens that aren't shown (all six pages at once used ~50 KB) is what overflowed the pool.
