# Site Baseline and Alerts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The station learns what is normal for its own site, by frequency and by hour of day, and says when something new appears or something regular goes silent, through the existing alert channels.

**Architecture:** A `BaselineModel` in core keeps two learned profiles in `<dataDir>/baseline/baseline.db`. **RF cells** are fed from the sweep scanner's `Sweep` lines and give occupancy per frequency cell per hour of day. **Regulars** are fed from decoded events through an automation listener, and give presence per (group, ident) per hour of day. After a learning period, a check that runs once a minute emits `baseline.*` automation events. A new watchlist kind `Baseline` turns those events into ntfy, Telegram, webhook or desktop alerts using the existing channels, rate limits and quiet hours.

**Tech Stack:** C++20, `sweep/sweep.h`, bundled SQLite, `automation`, `watchlist`.

**Spec:** [roadmap](2026-10-28-differentiators-roadmap.md), item 3.

## Global Constraints

- C++20; no new libraries; storage only in `<dataDir>/baseline/baseline.db`.
- Cell width: 12.5 kHz below 30 MHz and 25 kHz above (fixed, so profiles from different sweep settings line up). Hour buckets: 24 (local time).
- Learning: alerts start after 7 days of data for that cell or ident's hour bucket. The UI shows "learning, N of 7 days".
- Thresholds (defaults, editable):
  - **New signal:** a cell with occupancy < 2 % over the learning data, occupied in 3 consecutive sweeps.
  - **Missing signal:** a cell occupied ≥ 80 % at this hour on ≥ 5 of the last 7 days, and absent for this whole hour.
  - **New ident:** an ident never seen at this site.
  - **Missing regular:** an ident present at this hour on ≥ 5 of the last 7 days, and absent for this whole hour.
- Events use family `FSignal`, types `baseline.new_signal`, `baseline.missing_signal`, `baseline.new_ident`, `baseline.missing_regular`.
- Commits as AxolDad, no attribution lines.

## Review Focus

1. The radio, antenna or gain changes, so every cell looks new: when more than 20 % of the swept cells trip "new" in one sweep, the model raises one `baseline.shift` event ("the whole band changed") and suppresses the individual alerts. The user can then relearn.
2. The station is off for a day: absent data is not "missing". Missing checks need sweeps covering that cell during that hour today.
3. Daylight-saving change: buckets use local time, and the 23- or 25-hour day neither double-counts nor skips a bucket's day count.
4. A noisy ident source (pager addresses, BLE MACs that rotate): New-ident alerts are off by default for `GPagers` and `GBluetooth` and on for aircraft, ships, APRS and mesh.
5. The database grows without bound: idents not seen for 90 days are pruned, the cell count is bounded by the swept span, and the size is shown in the window.

---

### Task 1: The model (pure, in memory)

**Files:**
- Create: `core/include/dect2/baseline/baseline.h`, `core/src/baseline/baseline.cpp`
- Test: `tests/baseline/test_baseline.cpp`; register `baseline` in CMakeLists (link `dect2core`)

**Interfaces:**
- Produces (namespace `dect2::baseline`):
  - `struct BaselineConfig { int learnDays = 7; double newOcc = 0.02; int newConfirm = 3; double missOcc = 0.80; int missDays = 5; double shiftFrac = 0.20; uint32_t newIdentGroups = ((1u << eye::kGroups) - 1) & ~((1u << eye::GPagers) | (1u << eye::GBluetooth)); };`
  - `struct Finding { enum Kind { NewSignal, MissingSignal, NewIdent, MissingRegular, Shift } kind; double time, freqHz = 0; int group = -1; std::string ident, label, detail; };`
  - `class BaselineModel { explicit BaselineModel(BaselineConfig = {}); std::vector<Finding> addSweep(const Sweep& s, int localHour, int localDay); void addEvent(const automation::Event& e, int localHour, int localDay); std::vector<Finding> hourEnd(int localHour, int localDay); int learnedDays() const; void relearn(); };`
  - `int cellOf(double hz);` and `double cellCentreHz(int cell);`, using the widths in Global Constraints.
  - `localDay` is the number of days since 1970 in local time. The caller converts once, so tests never depend on the time zone.

- [ ] **Step 1: Write the failing tests**:
  - Learning: 7 days of sweeps with a carrier at 145.500 MHz every hour, then day 8 adds 433.920 MHz in 3 consecutive sweeps. Expect exactly one `NewSignal` at 433.925 MHz (the cell centre), and none before day 8.
  - Missing: the same 145.5 carrier is absent all of hour 14 on day 8. Expect `hourEnd(14, 8)` gives one `MissingSignal`. With no sweeps at all in hour 14 on day 8, expect none.
  - Shift: day 8 with 30 % of cells newly occupied gives exactly one `Shift` and no `NewSignal`.
  - Idents: aircraft `4CA2D1` at hour 9 on days 1-7, absent on day 8, gives `MissingRegular`. A never-seen aircraft on day 8 gives `NewIdent`. A never-seen pager address gives nothing.
  - `cellOf(145.5e6) == cellOf(145.51e6)`, and `cellOf(145.5e6) != cellOf(145.53e6)`.
- [ ] **Step 2: Run** `cmake --build build --target test_baseline && ./build/test_baseline`. Expect FAIL.
- [ ] **Step 3: Implement.**
  - Per cell, per hour bucket: keep a ring of the last 14 day-records `{day, sweeps, occupiedSweeps}`.
  - Per (group, ident): keep the set of (day, hour) presence for the last 14 days, plus firstSeen.
  - A cell counts as occupied in a sweep when any `Sweep::signals` entry overlaps it. A cell counts as swept when it lies inside the sweep's range.
- [ ] **Step 4: Run.** Expect PASS.
- [ ] **Step 5: Commit** `baseline: learn the site's normal signals and regulars by hour`

### Task 2: Persistence

**Files:**
- Modify: `core/include/dect2/baseline/baseline.h`, `core/src/baseline/baseline.cpp`
- Test: `tests/baseline/test_baseline.cpp`

**Interfaces:**
- Produces: `bool BaselineModel::open(const std::string& path, std::string& err);` (schema below, WAL mode); `void BaselineModel::flush();` (one transaction, called by the owner every 60 s and on close); `void prune(int localDay);` (idents older than 90 days); `struct BaselineStats { int cells, idents, learnedDays; int64_t bytes; }; BaselineStats stats() const;`
- Schema: `cells(cell INTEGER, hour INTEGER, day INTEGER, sweeps INTEGER, occ INTEGER, PRIMARY KEY(cell,hour,day))`, `idents(grp INTEGER, ident TEXT, label TEXT, first REAL, PRIMARY KEY(grp,ident))`, `seen(grp INTEGER, ident TEXT, day INTEGER, hours INTEGER /* 24-bit mask */, PRIMARY KEY(grp,ident,day))`, `meta(k TEXT PRIMARY KEY, v TEXT)`.

- [ ] **Step 1: Write the failing test**: learn 7 days, `flush`, destroy, `open` the same file in a new model, then day 8's new carrier still gives `NewSignal` (the learning survived). `prune(200)` removes an ident last seen on day 8.
- [ ] **Step 2: Run.** Expect FAIL.
- [ ] **Step 3: Implement** with the bundled SQLite and prepared statements. Day rows older than 14 days are deleted on `flush`.
- [ ] **Step 4: Run.** Expect PASS.
- [ ] **Step 5: Commit** `baseline: keep the learning in baseline.db`

### Task 3: Wiring: sweeps, events, findings as automation events

**Files:**
- Create: `core/include/dect2/baseline/baseline_service.h`, `core/src/baseline/baseline_service.cpp`
- Modify: `core/src/automation/catalog.cpp` (add the five `baseline.*` types with fields and descriptions, family `FSignal`); the sweep scanner owner that calls `SignalLog::update` (`core/src/sweep/sweep.cpp:~230`, where `signal.new` is emitted) also calls `BaselineService::sweep(s)`
- Test: `tests/baseline/test_baseline.cpp`

**Interfaces:**
- Consumes: plan 1 `Engine::addListener`. If plan 1 has not landed, add that listener API in this task (plan 1 Task 1 as written).
- Produces: `class BaselineService { bool start(const std::string& dataDir, std::string& err); void stop(); void sweep(const Sweep&); BaselineModel& model(); };` and `BaselineService& globalBaseline();`. It owns a 60 s timer thread that runs `hourEnd` at each hour change, `flush` and `prune`. Event fields: `freq` (Hz), `group`, `ident`, `label`, `detail`, `learned_days`.

- [ ] **Step 1: Write the failing test**: a service started on a temp dir, fed 8 days of synthetic sweeps through `sweep()` (with the hour and day injected through a test clock: `void setClockForTest(std::function<std::pair<int,int>(double)>)`). A listener on `FSignal` sees one `baseline.new_signal` with `freq` 433925000.
- [ ] **Step 2: Run.** Expect FAIL.
- [ ] **Step 3: Implement.** Findings become `automation::Event`s through `automation::emit`. Events that carry `f["site"]` (plan 1) are learned per site: the model key gets a site prefix, so one collecting station keeps one baseline per site.
- [ ] **Step 4: Run** `ctest --test-dir build -R "baseline|automation|sweep"`. Expect PASS.
- [ ] **Step 5: Commit** `baseline: a service that learns from sweeps and events and raises baseline.* events`

### Task 4: Baseline watchlist kind and the window

**Files:**
- Modify: `core/include/dect2/watchlist/watchlist.h` (`Kind::Baseline`; matching on the finding types the list ticks), `core/src/watchlist/watchlist.cpp`, `app/watchlist/watchlist_ui.cpp` (edit the kind), `app/main_start.cpp` (start `globalBaseline()` with `plat::dataDir()`)
- Create: `app/panels/baseline_ui.cpp` (`void baselineWindow(App&)`, `bool baselineOpen`), `docs/site-baseline.md`
- Modify: `app/ui/draw_ui.cpp`, `app/ui/ui2_shell.cpp` (menu "Baseline"), `command_palette.cpp`, `CMakeLists.txt` `APP_SOURCES`, `README.md` (one feature line)
- Test: `tests/watchlist/test_watchlist.cpp`

- [ ] **Step 1: Write the failing test**: a `Baseline` list ticking `new_signal` matches a `baseline.new_signal` event and not a `baseline.missing_regular` one. The webhook body has `kind: "baseline"` and `summary` set to the finding's detail.
- [ ] **Step 2: Run.** Expect FAIL. Implement. Run again. Expect PASS.
- [ ] **Step 3: Build the window.**
  - Show the learning progress ("learning, 4 of 7 days"), today's findings with Accept (marks the cell or ident as normal) and the database size.
  - Show a heat strip of occupancy by hour for a frequency the user picks.
  - Add a Relearn button behind a confirmation.
  - Add a hint: "Run the sweep scanner over the bands you care about; the baseline learns from it".
- [ ] **Step 4: Write `docs/site-baseline.md`**: what is learned, the thresholds, the learning time, the alert setup and the limits. Run the fast suite `ctest --test-dir build -LE "slow|timing" -j4`. Expect PASS.
- [ ] **Step 5: Commit** `Baseline window and alerts: what is new or missing at this site`
