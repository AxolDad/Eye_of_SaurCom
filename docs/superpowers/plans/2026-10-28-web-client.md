# Web Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A native browser client, served by the station itself, that works on a phone and lets several people watch at once: the Eye (timeline, contacts, map), the spectrum and waterfall, audio, and the message search; retuning only with a control token.

**Architecture:** The existing remote API grows static-file serving from files embedded at build time (`web/` → a generated header), read endpoints for the Eye and the message database shared by the headless server and the desktop app through one `ExtraApi` helper, and a second token kind for viewers. The page is plain HTML/CSS/JS with no external assets (the API sends a strict CSP); live data comes over the existing WebSocket (spectrum, audio, telemetry) plus plan 1's `events` subscription.

**Tech Stack:** C++20, `core/src/net/remote_api.cpp`, CMake `file(READ ... HEX)` embedding, vanilla JS (ES2020), Canvas 2D.

**Spec:** [roadmap](2026-10-28-differentiators-roadmap.md), item 2. Requires plan 1 tasks 1–3 (event stream).

## Global Constraints

- C++20; no new third-party libraries; no JS framework, no CDN, no external fonts or map tiles (CSP at `remote_api.cpp:224` stays as strict).
- One page set for both hosts: `server/web_ui.h` `kIndexHtml` is replaced by the embedded `web/` files.
- Viewer tokens cannot call any POST endpoint (403) and cannot open audio beyond the existing 4-listener cap.
- Phone layout works at 360 px wide with no horizontal scroll.
- Commits as AxolDad, no attribution lines.

## Review Focus

1. A viewer token POSTing `/api/v1/frequency` or `/api/v1/x/demod`: 403 with `{"error":"view-only token"}`, nothing changes.
2. A path with `..`, `%2e%2e`, a backslash or a NUL in it: 404, never a file outside the embedded set.
3. Losing the WebSocket (phone sleeps, Wi-Fi changes): the page reconnects with backoff and resubscribes with `"since"` so the timeline has no gap and no duplicates.
4. Remote event text containing `<script>` or HTML: shown as text (`textContent`), never parsed.
5. A slow phone: spectrum fps drops to what it can draw (the client asks for `fps` it measured); the server never queues more than one spectrum frame per client.

---

### Task 1: Embedded static files

**Files:**
- Create: `web/index.html`, `web/app.css`, `web/app.js` (move the current `kIndexHtml` content here first, unchanged in behaviour), `cmake/embed_web.cmake`
- Modify: `CMakeLists.txt` (generate `${CMAKE_BINARY_DIR}/gen/web_assets.h` before `dect2core`), `core/include/dect2/net/remote_api.h` (`virtual bool staticFile(const std::string& path, std::string& body, std::string& contentType)` with a default that serves the embedded set), `core/src/net/remote_api.cpp` (route GET of non-`/api/` paths), delete `server/web_ui.h`
- Test: `tests/net/test_remote_api.cpp`

**Interfaces:**
- Produces: `struct WebAsset { const char* path; const unsigned char* data; size_t size; const char* type; };` `const WebAsset* findWebAsset(const std::string& path);` in the generated header's companion `core/src/net/web_assets.cpp`. `/` maps to `/index.html`.

- [ ] **Step 1: Write the failing test**: GET `/app.js` returns 200 with `Content-Type: text/javascript; charset=utf-8`; GET `/` equals GET `/index.html`; GET `/../CMakeLists.txt`, `/%2e%2e/x`, `/a\\b` and `/nope` return 404.
- [ ] **Step 2: Run** `ctest --test-dir build -R remote_api` — FAIL.
- [ ] **Step 3: Implement**: `embed_web.cmake` reads each file with `file(READ ... HEX)` and writes one `static const unsigned char` array per file plus the table; types by extension (`.html`, `.css`, `.js`, `.svg`, `.png`). Path lookup is an exact table match after decoding, so traversal cannot reach the disk at all.
- [ ] **Step 4: Run** — PASS; open `http://127.0.0.1:8765/` from `saurcom-server` and check the old page still works.
- [ ] **Step 5: Commit** `remote API: serve the web client from files embedded at build time`

### Task 2: View and control tokens

**Files:**
- Modify: `core/include/dect2/net/remote_api.h` (`RemoteApiConfig::viewToken`), `core/src/net/remote_api.cpp` (auth), `server/server_config.cpp` (`view_token=` key), `app/platform/remote_api_app.cpp` and the app's remote API settings UI (a second "view-only token" field with Generate)
- Test: `tests/net/test_remote_api.cpp`

**Interfaces:**
- Produces: request role `enum class Role { None, View, Control }`; loopback stays Control as today; `token` → Control, `viewToken` → View; both at least 16 characters.

- [ ] **Step 1: Write the failing test**: with both tokens set, the view token GETs `/api/v1/status` (200), POSTs `/api/v1/frequency` (403, body `{"error":"view-only token"}`), opens the WebSocket and subscribes to spectrum and events (works); the control token POSTs (200); a wrong token is 401 after the existing 300 ms delay.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement**; the WebSocket subscription keeps the role and ignores any control keys from a View client.
- [ ] **Step 4: Run** — PASS.
- [ ] **Step 5: Commit** `remote API: view-only tokens`

### Task 3: Read endpoints for the Eye and messages

**Files:**
- Create: `core/include/dect2/net/extra_api.h`, `core/src/net/extra_api.cpp`
- Modify: `server/server_host.cpp:511-528` and `app/platform/remote_api_app.cpp` (`handleExtra` calls `extraApi(...)` first); the headless server feeds `eye::global()` and opens `msgdb::global()` like the app does (`app/panels/eye_ui.cpp:365`, `app/panels/msgdb_ui.cpp:85`)
- Test: `tests/net/test_extra_api.cpp`

**Interfaces:**
- Consumes: `eye::Overview` (`timeline`, `contacts`, `stats`), `msgdb::Db::query(const Filter&)`.
- Produces: `int extraApi(const std::string& method, const std::string& path, const std::map<std::string, JsonVal>& query, std::string& json);` (0 = not mine). Routes, GET only:
  - `/api/v1/x/eye/timeline?max=200&groups=<mask>&q=<text>&site=<name>` → `{"rows":[{seq,time,group,mode,ident,label,text,freq,first,lat,lon,site}]}`
  - `/api/v1/x/eye/contacts?age=3600&groups=&q=` → `{"contacts":[...]}`
  - `/api/v1/x/eye/stats?window=600` → `{"groups":[{"name","active","events"}]}`
  - `/api/v1/x/msgs?q=&mode=&since=&until=&limit=100` (limit capped at 500) → `{"msgs":[...],"total":N}`
- Note: the query string reaches `handleExtra` today only as the POST body map; add query-string parsing for GET in `remote_api.cpp` into the same map (keys and values URL-decoded, flat).

- [ ] **Step 1: Write the failing tests**: ingest three Eye events, CHECK `timeline?max=2` returns the two newest; `groups` mask filters; `q` matches label; msgs against a temp `messages.db` with 3 records returns `total` 3 and honours `limit=1`; `limit=100000` is capped at 500; an unknown path returns 0.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** with `json::dump` for output.
- [ ] **Step 4: Run** — PASS.
- [ ] **Step 5: Commit** `remote API: read the Eye and the message database`

### Task 4: The page: Eye, map, live updates

**Files:**
- Modify: `web/index.html`, `web/app.css`, `web/app.js` (split into `web/eye.js`, `web/spectrum.js`, `web/audio.js`, `web/api.js` if `app.js` passes 400 lines)

**Interfaces:**
- Consumes: Task 3 endpoints, plan 1's `{"events":true,"since":N}` subscription.

- [ ] **Step 1: Build the views**, as tabs on a phone and side-by-side panes at 1000 px and wider:
  - **Eye**: group chips with counts (from `stats`), a filter box, the timeline (newest first, initial load from `timeline`, then live from `event` frames, deduplicated by `seq`), a contacts list.
  - **Map**: a Canvas with an equirectangular projection centred on the station, range rings at 25/50/100/200 km, contacts with positions as group-coloured dots, labels on tap. No tiles.
  - **Spectrum / waterfall**, **Audio**, **Tune** (Tune hidden for view tokens): moved from the old page.
  - **Messages**: the search box over `/x/msgs`.
- [ ] **Step 2: Reconnect**: on close, retry after 1, 2, 4 … 30 s; resubscribe with `since` = last seq seen.
- [ ] **Step 3: Manual check** in Chromium at 360×740 and 1440×900 (Playwright is installed: `require('playwright')`), with a test host replaying an ADS-B and AIS file: timeline fills, map shows dots, no horizontal scroll at 360 px, killing and restarting the server leaves no gap. Save screenshots to the scratchpad.
- [ ] **Step 4: Run** `ctest --test-dir build -R remote_api` — PASS (the asset table changed).
- [ ] **Step 5: Commit** `web client: the Eye, a map, messages; works on a phone`

### Task 5: Docs and packaging

**Files:**
- Create: `docs/web-client.md`
- Modify: `docs/headless-server.md` (the new page and the view token), `packaging/docker/README.md` and `packaging/proxmox/README.md` (the native page on port 8765 next to the Selkies desktop on 3001; the Proxmox container's `compose.yaml` publishes 8765 when the user enables it), `README.md` (one feature line)

- [ ] **Step 1: Write the docs**: open `http://<host>:8765/`, the two tokens and what each may do, phone use, what is not in the web client yet (per-viewer channels).
- [ ] **Step 2: Run** the full fast suite `ctest --test-dir build -LE "slow|timing" -j4` — PASS.
- [ ] **Step 3: Commit** `docs: the web client`
