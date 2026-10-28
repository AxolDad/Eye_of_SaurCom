# Setup: Station Check and Listening Plan Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The existing setup wizard gains two steps:
- **Check my station.** A short sweep of the radio says what the antenna and site can hear.
- **Your listening plan.** A ranked list of things worth listening to here, with the reason and when for each, which the user can apply in one click.

**Architecture:**
- `planListening()` is a pure function in core. It turns the wizard's answers (region, location, goals) plus the radio's tuning range, the station-health results and the satellite passes into `PlanItem`s.
- The wizard runs `runHealthCheck` on a worker thread, calls the planner, and on Apply:
  - opens the first item's mode;
  - adds the timed items (satellite passes) to the pass scheduler;
  - saves the plan so the "Get me listening" wizard and the Eye can show it later.

**Tech Stack:** C++20, `sweep/station_health.h`, `geo/pass_sched.h`, `prop/prop.h`, `bandplan/bandplan.h`, ImGui wizard (`app/ui/wizard.cpp`).

**Spec:** [roadmap](2026-10-28-differentiators-roadmap.md), item 5.

## Global Constraints

- The station check is optional and skippable. It must finish in under 60 s on a 24-1766 MHz RTL-SDR with the default `HealthConfig`, and it shows its progress and a Cancel button.
- The check never transmits and never changes the saved gain.
- The plan is advice. Apply changes only what the user can see in the list.
- Every user-visible string goes through `tr()` / `N_()` like the rest of the wizard.
- Commits as AxolDad, no attribution lines.

## Review Focus

1. A radio that cannot cover a goal's band (an RTL-SDR and HF FT8 without an upconverter): the item is shown greyed, with "outside this radio's range (24-1766 MHz)", never offered as Apply.
2. No location given: no satellite pass times, no ADS-B range. The plan still works and says "add your location for satellite passes".
3. The check finds overload (`FindCode::Overload`, `StrongOutOfBand`): the plan's first line is the fix (lower the gain, or add a band-stop filter on <band>) before any listening item.
4. The user cancels the check halfway: the plan uses the bands that were measured and marks the others "not checked".
5. Re-running the wizard later: it replaces the saved plan and does not duplicate scheduled passes (passes are keyed by satellite and AOS time).

---

### Task 1: The planner (pure)

**Files:**
- Create: `core/include/dect2/plan/listen_plan.h`, `core/src/plan/listen_plan.cpp`
- Test: `tests/plan/test_listen_plan.cpp`. Register `listen_plan` in CMakeLists (link `dect2core`).

**Interfaces:**
- Consumes: `sweep::BandResult`, `sweep::HealthReport` (`station_health.h:50,114`), `geo::SchedPass` (`pass_sched.h:12`).
- Produces:
  - `struct PlanInputs { std::string region; /* the wizard's region id */ unsigned goals; /* the wizard's kGoals bit mask */ bool hasLocation = false; double lat = 0, lon = 0; double minHz = 0, maxHz = 0; /* the radio */ std::vector<sweep::BandResult> bands; sweep::HealthReport health; std::vector<geo::SchedPass> passes; /* next 24 h */ };`
  - `struct PlanItem { std::string modeId, title, why, when; double freqHz = 0; int score = 0; bool possible = true; bool isFix = false; };`
  - `std::vector<PlanItem> planListening(const PlanInputs&);`. The result is sorted by score, fixes first.
- Scoring rules (they decide the order; the tests pin them):
  - A ticked goal: +50.
  - Its band measured with a reference present (`RefLevel::Ok`): +20. `Weak`: +5.
  - Its band `nSignals > 0`: +10.
  - Outside `minHz..maxHz`: `possible = false`, score 0.
  - Satellites: one item per pass in the next 24 h with max elevation ≥ 20°; `when` is the local AOS time; +15 when the elevation is ≥ 45°.
  - Health findings `Overload`, `StrongOutOfBand`, `AdcLimited`, `NoiseExcess` become `isFix` items with score 1000 minus their index.
- Mode ids come from the wizard's `kGoals` (`app/ui/wizard.cpp:36-46`): `fm`, `tv`, `adsb`, `ais`, `meteor`, `ft8`, `dmr`, `ism`, `specan`. Move that table into `listen_plan.cpp` as `const PlanGoal kPlanGoals[]` with `{label, modeId, startHz, stopHz}` so the wizard and the planner share it.

- [ ] **Step 1: Write the failing tests**:
  - **RTL range.** Goals Aircraft + Amateur radio, radio 24-1766 MHz. Expect `adsb` possible, and `ft8` (14.074 MHz) `possible == false` with a `why` containing "outside this radio's range".
  - **Overload first.** A health report with `Overload` on FM. Expect the first item to be `isFix` and to mention FM.
  - **Passes.** Two passes, max elevation 15° and 60°. Expect one satellite item (the 60° pass) with the +15.
  - **No location.** Expect no satellite items, and one info item whose `why` contains "add your location".
  - **Order.** A ticked goal with `RefLevel::Ok` outranks a ticked goal without measurement.
- [ ] **Step 2: Run** `cmake --build build --target test_listen_plan && ./build/test_listen_plan`. Expect FAIL.
- [ ] **Step 3: Implement.** Point `app/ui/wizard.cpp` at `kPlanGoals`, removing its own `kGoals`.
- [ ] **Step 4: Run** `ctest --test-dir build -R "listen_plan|station_health|pass_sched"`. Expect PASS. Build the app.
- [ ] **Step 5: Commit** `plan: a listening plan from the station's goals, radio and measured bands`

### Task 2: Wizard steps "Check my station" and "Your plan"

**Files:**
- Modify: `app/ui/wizard.cpp` (two steps between "goal" and "done"), `app/app.h` (wizard state: the check's thread, its progress, its results, the plan), `app/state/prefs.cpp` (key `listenPlan`: the applied plan as JSON via `json_mini`)
- Modify: the pass scheduler's app side (find where `planPasses` is called in `app/`, and add `addPlannedPasses(const std::vector<geo::SchedPass>&)`, which skips any satellite+AOS already queued)
- Test: the planner is covered by Task 1. This task is UI plus a dedupe helper: add `tests/geo/test_pass_sched.cpp` case `dedupe`, which adds the same pass twice and expects one in the queue.

**Interfaces:**
- Consumes: Task 1 `planListening`, `kPlanGoals`; `runHealthCheck` (`station_health.h:134`) through an `IqSweepSource(const DeviceInfo&, const TuneSettings&)` (`sweep.h:125`) on the selected radio; `assessHealth`.

- [ ] **Step 1: Write the failing dedupe test.** Run it and expect FAIL. Implement. Run again and expect PASS.
- [ ] **Step 2: Build "Check my station".**
  - Intro text: "About 30 seconds. Leave the antenna where it will stay."
  - Start and Skip buttons; a progress bar with band names; Cancel.
  - Results as a small table: band, noise floor, signals, reference station Ok/Weak/Absent.
  - The health findings in the user's language. Reuse the station health window's text function; find it in `app/` beside `healthJson`.
  - The check needs the radio free. If a mode is running, say "Stop the running mode to check the station" with a Stop button.
- [ ] **Step 3: Build "Your plan".**
  - The ranked items. Fixes have a warning colour and come first. Impossible items are greyed, with their reason.
  - A tick box per possible item, all ticked by default.
  - Apply does three things: it starts the top ticked item's mode with the existing `applyGoal`; it adds the ticked satellite passes; it saves `listenPlan`.
  - Then the done step, with "The plan is under Help > My listening plan".
- [ ] **Step 4: Manual check.** Use a recorded `.cu8` file source (the radio list offers file sources), so the check runs without hardware. Then check with an RTL-SDR if one is at hand. Record which was used.
- [ ] **Step 5: Commit** `setup wizard: check the station, then a listening plan`

### Task 3: "My listening plan" afterwards, and docs

**Files:**
- Create: `docs/listening-plan.md`
- Modify: `app/ui/ui2_shell.cpp` (Help > "My listening plan", which shows the saved plan with Re-check and Open buttons per item); `app/ui/listen_wizard.cpp` (its preset step offers the saved plan's items first); `README.md` (one feature line in the setup section)

- [ ] **Step 1: Build the menu entry.**
  - It reads `listenPlan` and lists the items with their `why` and `when`.
  - Open per item starts that mode.
  - Re-check reopens the wizard at the station-check step.
- [ ] **Step 2: Write `docs/listening-plan.md`**: what the check measures, how the plan is ranked, what Apply changes, and how to re-run it.
- [ ] **Step 3: Run** the fast suite `ctest --test-dir build -LE "slow|timing" -j4`. Expect PASS.
- [ ] **Step 4: Commit** `My listening plan: the plan stays one click away`
