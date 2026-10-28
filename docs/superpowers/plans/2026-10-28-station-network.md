# Station Network Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Several Eye of SaurCom installs feed one station's Eye, so one map, timeline and contact list shows what every site hears, with cross-site bearing fixes.

**Architecture:** Each station keeps its decoded events in an `EventRing` fed by a new fan-out listener on the automation bus. The existing remote API WebSocket gets an `events` subscription that streams the ring as JSON lines. A collecting station runs one `StationLink` per remote site: it connects with `WsClient`, tags each event with `site`, and submits it to its own bus, so the Eye, message database and watchlists get remote events with no changes of their own. Events carrying `site` are never exported again, so links cannot loop.

**Tech Stack:** C++20, in-house HTTP/WebSocket (`core/src/net/remote_api.cpp`, `core/src/net/ws_client.h`), `automation/json_mini.h`, `df/position.h`, ImGui.

**Spec:** [roadmap](2026-10-28-differentiators-roadmap.md), item 1; the "differentiators" answer in the 2026-10-28 conversation.

## Global Constraints

- C++20; no new third-party libraries.
- Off by default: no export without a remote API token, no links without the user adding a site.
- Links are `ws://` on the LAN or a VPN (no TLS client exists); the UI says so.
- An event with an `f["site"]` field is never exported by `EventRing`.
- Tests: `tests/common/test_util.h` `CHECK`; register with `add_executable` + `target_link_libraries(... PRIVATE dect2core)` + `add_test`; socket tests go in the `timing` label list (CMakeLists ~1734).
- Commits as AxolDad, no attribution lines.

## Review Focus

1. A remote site that is down, refuses the token or drops mid-stream: the link retries with backoff (2 s doubling to 60 s), shows the reason, and never blocks the UI or the decoders.
2. A burst of events faster than the link reads: the ring overwrites the oldest, the collector learns how many it missed (`gap` count), nothing grows without bound.
3. Two sites reporting the same aircraft or ship: one contact in the Eye with both sites listed, not two contacts.
4. Clock skew between sites: events keep the sender's time; the Eye orders by it, and a site whose clock is more than 30 s off its link's receive time is flagged.
5. Field text from a remote site is untrusted: it goes through the same `kMaxFieldText` cut and the Eye renders it as text only.

---

### Task 1: Fan-out listeners on the automation bus

**Files:**
- Modify: `core/include/dect2/automation/engine.h:73-84` (add the listener API and its mask to `wants()`), `core/src/automation/engine.cpp` (`submit` calls listeners)
- Test: `tests/automation/test_automation.cpp` (new case)

**Interfaces:**
- Produces: `int Engine::addListener(std::function<void(const Event&)> f, uint32_t familyMask);` returns an id > 0; `void Engine::removeListener(int id);`. Same rules as `setTap`: decoder thread, must not block. `wants()` ORs in the union of listener masks.

- [ ] **Step 1: Write the failing test** `testListeners()`: add two listeners (masks `1<<FAdsb` and `1<<FAis`), submit one `adsb.*` and one `ais.vessel` event, `drain()`; CHECK each listener saw exactly its family; CHECK `wants(FAis)`; remove the AIS listener; CHECK `!wants(FAis)` (no rule wants AIS in the test).
- [ ] **Step 2: Run** `cmake --build build --target test_automation && ./build/test_automation` — expect FAIL (no `addListener`).
- [ ] **Step 3: Implement** a `std::vector<{int id; uint32_t mask; fn}>` under a mutex plus an atomic `listenMask_` recomputed on add/remove; `submit` copies the matching functions under the lock and calls them outside it.
- [ ] **Step 4: Run** the test — PASS. Run `ctest --test-dir build -R "automation|eye|msgdb|watchlist"` — PASS.
- [ ] **Step 5: Commit** `automation: fan-out listeners beside the four fixed hooks`

### Task 2: EventRing and JSON lines

**Files:**
- Create: `core/include/dect2/net/event_ring.h`, `core/src/net/event_ring.cpp`
- Test: `tests/net/test_event_ring.cpp`; register `event_ring` in CMakeLists next to `test_remote_api` (~1138)

**Interfaces:**
- Produces:
  - `std::string eventToJson(const automation::Event& e);` — `{"type":..,"time":..,"mode":..,"freq":..,"f":{...}}` via `json::dump`.
  - `bool eventFromJson(const std::string& line, automation::Event& out, std::string& err);` — rejects a missing `type`, non-object `f`, nested values in `f`; cuts strings at `kMaxFieldText`.
  - `class EventRing { explicit EventRing(size_t cap = 4096); void push(const automation::Event&); /* skips events with f["site"] */ size_t since(uint64_t& cursor, std::vector<std::string>& lines, size_t max, uint64_t& missed) const; uint64_t head() const; };`
  - `EventRing& globalEventRing();`

- [ ] **Step 1: Write the failing tests**:
  - round trip of an event with num/str/bool fields: equal type, time, mode, freq and fields;
  - `eventFromJson("{\"f\":{}}")` false; nested `f` value false; a 5000-char string comes back 2000 chars;
  - ring of cap 4: push 6, `since(cursor=0)` returns 4 lines and `missed == 2`, cursor == 6; a second `since` returns 0 lines;
  - an event with `f["site"]="b"` is not pushed (`head()` unchanged).
- [ ] **Step 2: Run** — FAIL (missing header).
- [ ] **Step 3: Implement**; the ring stores the JSON text (serialise once on push), guarded by a mutex; `push` must not allocate beyond the one string.
- [ ] **Step 4: Run** — PASS.
- [ ] **Step 5: Commit** `net: EventRing, events as JSON lines`

### Task 3: `events` WebSocket subscription

**Files:**
- Modify: `core/include/dect2/net/remote_api.h:44-66` (optional host call), `core/src/net/remote_api.cpp:311-373` (subscribe keys and the send loop), `server/server_host.cpp` and `app/platform/remote_api_app.cpp` (feed `globalEventRing()` from a listener with all families, implement the host call)
- Test: `tests/net/test_remote_api.cpp` (new case)

**Interfaces:**
- Consumes: Task 1 `addListener`, Task 2 `EventRing`.
- Produces: `virtual size_t RemoteApiHost::eventLines(uint64_t& cursor, std::vector<std::string>& lines, uint64_t& missed) { return 0; }`. Client subscribe keys `"events": true` and optional `"since": N`. Server text frames: `{"type":"event","seq":N,"ev":{...}}` per event and `{"type":"gap","missed":N}` when the ring overwrote. At most 200 events per 100 ms tick per client.

- [ ] **Step 1: Write the failing test**: a test host returning three lines; WebSocket client subscribes `{"events":true}`; CHECK three `"type":"event"` frames in order with seq 1..3; a host reporting `missed=5` produces one `gap` frame with `"missed":5`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** in the existing per-client send loop beside telemetry and spectrum; events need the token like every other API call.
- [ ] **Step 4: Run** `ctest --test-dir build -R remote_api` — PASS.
- [ ] **Step 5: Commit** `remote API: stream decoded events over the WebSocket`

### Task 4: StationLink (the collector)

**Files:**
- Create: `core/include/dect2/net/station_link.h`, `core/src/net/station_link.cpp`
- Test: `tests/net/test_station_link.cpp` (label `timing`)

**Interfaces:**
- Consumes: `WsClient` (`core/src/net/ws_client.h`), Task 2 `eventFromJson`, `automation::emit`.
- Produces:
  - `struct SiteConfig { std::string name, host; int port = 8765; std::string token; bool on = true; };`
  - `struct SiteStatus { int state; /* 0 off, 1 connecting, 2 live, 3 retrying */ std::string lastError; uint64_t events = 0, missed = 0; double lastEventAt = 0, clockSkewSec = 0, lat = NAN, lon = NAN; };`
  - `class StationLinks { void configure(const std::vector<SiteConfig>&); std::vector<std::pair<SiteConfig, SiteStatus>> status() const; void stop(); };` and `StationLinks& globalLinks();`
  - Each received event gets `f["site"] = name` and is submitted with `automation::emit`. The link reads the remote site's position from `GET /api/v1/status` (`lat`/`lon` if present) once per connect.

- [ ] **Step 1: Write the failing test**: start a `RemoteApiServer` on a free loopback port with a host whose `eventLines` yields two events; configure one site; wait up to 5 s; CHECK a local listener saw both events with `f["site"] == "test"`; stop the server; CHECK the status turns to 3 (retrying) with a non-empty `lastError`; a wrong token gives state 3 and an error containing `401`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement**: one thread per site; backoff 2, 4, 8 … 60 s; `clockSkewSec` = receive time − event time, smoothed.
- [ ] **Step 4: Run** — PASS 5 times in a row (`ctest -R station_link --repeat until-fail:5`).
- [ ] **Step 5: Commit** `net: StationLinks, a collector for other stations' events`

### Task 5: Per-site Eye and cross-site bearing fixes

**Files:**
- Modify: `core/include/dect2/eye/eye.h` (`Row::site`, `Contact::sites` as `std::vector<std::string>`, a `site` filter argument), `core/src/eye/eye.cpp`
- Create: `core/include/dect2/df/xfix.h`, `core/src/df/xfix.cpp`; DF window emits `df.bearing` (find where the DF UI shows its bearing in `app/df/` and emit there; add `df.bearing` to `catalog.cpp` with family `FSignal`)
- Test: `tests/eye/test_eye.cpp`, `tests/df/test_xfix.cpp`

**Interfaces:**
- Consumes: Task 4 events with `f["site"]`, `fixPositionRobust` (`df/position.h:35`).
- Produces: `df.bearing` event fields `bearing_deg`, `sigma_deg`, `lat`, `lon` (the station's position); `struct CrossFix { double freqHz, time; Fix fix; std::vector<std::string> sites; };` `class CrossFixer { void add(const automation::Event& bearingEvent); std::vector<CrossFix> fixes(double now) const; };` — groups bearings within ±25 kHz and ±5 s from at least 2 sites, newest bearing per site.

- [ ] **Step 1: Write the failing tests**:
  - Eye: the same `adsb` ident from site "" (local) and site "b" → `contacts()` returns one contact with `sites == {"", "b"}`; `timeline(..., site="b")` returns only b's rows.
  - CrossFixer: two stations 10 km apart with exact bearings to a point → one fix within 50 m of it; bearings 30 kHz apart → no fix; one site only → no fix.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement**; contacts stay keyed `group|ident`, `sites` is the sorted union. Add `Group GFixes` after `GDrones` and before `kGroups`, with `groupName` "Located"; each cross fix becomes a `GFixes` row (ident = frequency in kHz, text = the sites and the 95 % error in metres) and moves its contact.
- [ ] **Step 4: Run** `ctest --test-dir build -R "eye|xfix|df"` — PASS.
- [ ] **Step 5: Commit** `eye: one picture across sites; bearings from several sites become fixes`

### Task 6: Stations window and docs

**Files:**
- Create: `app/panels/stations_ui.cpp` (`void stationsWindow(App&)`, `bool stationsOpen` in `app/app.h`), `docs/station-network.md`
- Modify: `app/ui/draw_ui.cpp:179-230` (call it), `app/ui/ui2_shell.cpp` (menu entry "Stations"), `command_palette.cpp`, `app/state/prefs.cpp` (key `stationSites`, the sites as JSON), `app/panels/eye_ui.cpp` (a site chip row and the site name on each row), `CMakeLists.txt` `APP_SOURCES`, `README.md` (one feature line)

- [ ] **Step 1: Build the window**: a table of sites (name, host:port, state, events, missed, last event, clock skew, error), Add / Edit / Remove, and a line "Links are not encrypted: use them on your own network or a VPN". The token field is a password field; saved prefs keep it (same as `watchlists.json`: file mode 0600).
- [ ] **Step 2: Manual check**: run two copies (`DECT2_DATA_DIR` different, the second with the remote API on another port and a token), play an ADS-B test file in the second, add it as a site in the first; the Eye shows its aircraft with the site chip.
- [ ] **Step 3: Write `docs/station-network.md`**: what it does, how to enable the API token on each site, adding sites, what crosses the link, what does not (audio, IQ), limits (no TLS yet, no TDOA).
- [ ] **Step 4: Run** the full fast suite `ctest --test-dir build -LE "slow|timing" -j4` — PASS.
- [ ] **Step 5: Commit** `Stations window: link other installs into this Eye`
