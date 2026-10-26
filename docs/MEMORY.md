# Memory budget

The board is an ESP32-WROOM without PSRAM: about 300 KB of RAM in total, split by the chip into a
few separate regions. Every feature competes for that RAM. This document explains how memory is
planned, measured and guarded, and what to do when you add a feature.

## The model

There are three kinds of memory, and each feature uses one of them:

| Resource | What it is | Who uses it | What runs out |
|---|---|---|---|
| **Heap** (free bytes) | general RAM after WiFi, the LVGL pool and the draw buffer are reserved, about 104 KB | HTTPS fetches (TLS buffers, JSON), text of the data snapshots, the web server | the total |
| **Largest free block** | the biggest contiguous piece of the heap | TLS: one handshake needs pieces of roughly 17 KB and fails below about 40 KB largest block | contiguity: 80 KB free in small pieces is useless for TLS |
| **LVGL pool** | `LVGL_POOL_BYTES` (56 KB) reserved at boot from the *smaller* heap regions, so the big region stays whole for TLS | everything on screen: base UI (header, tab bar, banner, refresh screen), the current page, the status screen | the total; overflow spills into the heap and fragments it |

Two rules follow from this:

- **Transient heap users** (fetches) need the heap *and* a large block while they run. They must
  never run while something else holds that block.
- **Pool users** (pages) only exist while shown. During a slide, the old and the new page exist
  together, so the pool must hold the base UI plus two pages.

## The three mechanisms (`jira_status/memory.h`)

**1. The plan, checked at compile time.** `MEM_BUDGET` lists every feature with an estimate of its
peak use. `static_assert`s check:

```
heap:  services + 3 × snapshot text + fresh snapshot + heaviest fetch  ≤  HEAP_BASE_EST
pool:  base UI + 2 × biggest page                                     ≤  LVGL_POOL_BYTES
pool:  base UI + status screen + health page                          ≤  LVGL_POOL_BYTES
```

If a change breaks the plan, the build stops with `Memory plan: ...` instead of the board
crashing on the desk.

**2. Measurement at run time.** Every time a feature runs, the board measures it:

- a fetch: free heap before, minus the lowest free heap while it runs (sampled every 4 ms), plus
  the largest free block it started with and whether it ran out of memory;
- a page or the status screen: LVGL memory after building it minus before (pool plus overflow).

It logs `mem: fetch jira used 52k (estimate 60k, learned peak 52k)` and keeps the peaks in flash
(NVS, written only when they change notably), so the numbers survive restarts.

**3. Admission before running.** `mem_admit(feature)` compares what the feature needs with what is
free now:

- **needs** = the measured peak once it has 3 measurements, the larger of estimate and measurement
  before that, plus 15% and 2 KB;
- **largest block** (fetches) = the estimate (40 KB), lowered to the smallest block it has worked
  with, but always above the largest block it has *failed* with.

The callers act on the answer:

| Feature | When there's no room |
|---|---|
| status check (every minute, dashboard on screen) | skipped; it runs with the next refresh, when the dashboard is freed |
| Kuma check in place | the dashboard is freed for it, as in a full refresh |
| full refresh, after freeing the dashboard | the heap is too fragmented: the board restarts (except on the first refresh after boot, to avoid a loop) |
| next page | the current page is freed first and the new one appears without the slide |
| status screen | the dashboard page is freed first |

## Adding a feature

1. **Estimate.** Add a line to `MEM_BUDGET` in `memory.h` (and a `MemFeature` value). Heap features:
   one HTTPS request is about 46-54 KB plus its JSON document (ArduinoJson: roughly 1.2× the
   filtered JSON text). Pool features: about 120-200 bytes per LVGL object plus its text; count
   the objects.
2. **Check the plan.** It's checked when you compile: a `Memory plan` error means it doesn't fit.
   Then make it cheaper, build it only while shown, or decide what to give up.
3. **Measure.** Wrap it: `mem_begin(f)` / `mem_end(f, ok, err)`, and call `mem_admit(f)` before
   running it where there's a fallback (postpone, free something, skip).
4. **Verify on the board.** Open `http://jira-status.local/memory` after it has run a few times:
   estimate against measured peak, how many runs, whether there's room now. If the measurement is
   far from the estimate, correct the estimate so the plan stays honest.

## Reading `/memory`

- **After WiFi** should be close to `HEAP_BASE_EST`. If it drops (a library update, a new global),
  every heap feature has less room: update the constant and the plan re-checks.
- **Measured peak** higher than the estimate: raise the estimate.
- **OOM** above 0: that feature ran out of memory at least once; the block it failed with is
  remembered, and it won't be started below it again.
- **Room now: no** for a fetch while the dashboard is on screen is normal; it runs during the next
  refresh instead.
- **Forget measurements** clears the learned values, for example after a big change.

## Things that cost memory and are easy to miss

- An LVGL style change from a timer redraws the object every time even when the value is the same:
  that costs time, not memory, but large redraws starve everything else.
- `lv_anim` on a big object (a 150 px ring) redraws the whole object every frame. Move small
  objects instead.
- Every `String` on a long-lived struct is a separate heap block: they fragment the heap the TLS
  handshake needs. Keep the counts (`MAX_CARDS`, `MAX_MEETINGS`, ...) as small as the screen needs.
- Keeping screens that are not shown (all six pages at once used ~50 KB) is what overflowed the pool.
