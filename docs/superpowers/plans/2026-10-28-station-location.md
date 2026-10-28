# Station Location Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Set the station's position once and in an easy way. Every map and every range calculation uses it, and maps open at it.

**Architecture:**
- A core `StationLocation` holds:
  - the position and its source;
  - parsers for coordinates, Maidenhead locators and map links;
  - an NMEA/gpsd reader.
- The app keeps one instance and saves it to prefs. It reads the existing `adsbLat`/`adsbLon` keys at first start, so nothing is lost.
- Every screen that centres a map or computes a range reads the instance through `receiverLocation()` / `receiverLocationSet()`.
- The Proxmox/Docker installs pass `SAURCOM_LAT`/`SAURCOM_LON` (or `SAURCOM_LOCATOR`). These are used only while no location is saved.
- The remote API status reports the position, so linked stations know where each site is.

**Tech Stack:** C++20, ImGui, `geo/maidenhead.h` (`fromMaidenhead`), `parseCoordinates`, `rfi::parseNmea`, `json_mini`, the in-house
TCP client (for gpsd).

**Spec:** the design the user approved on 2026-10-28 in the conversation, without the IP lookup ("no IP lookup"):
1. **One "My station" setting:** latitude, longitude, height above sea level and antenna height. It has one Settings page and a location
   chip in the toolbar. Every map and range calculation uses it: ADS-B, AIS, APRS, sondes, mesh, DF, HFDL, the Eye map, satellite passes,
   propagation and NOTAMs. Maps open centred on it, and each map has a **Centre on me** button.
2. **Easy ways to set it:**
   - paste coordinates, a Maidenhead locator, or a Google or OpenStreetMap link;
   - right-click any map and choose "I'm here";
   - read it from a GPS receiver (NMEA over serial, or gpsd).
   - No IP-based lookup.
3. **Proxmox/Docker:** the installer takes `--location "52.1,-1.3"` (or a locator) and asks for it if it is not given. The container gets
   `SAURCOM_LAT`/`SAURCOM_LON`, which are used when no location is saved yet.
4. **Station network:** each station reports its position in its status, so the Stations window and the cross-site fixes know where
   every site is.

## Global Constraints

- C++20; no new libraries. Nothing leaves the station: no geocoding service and no IP lookup.
- Existing saved positions (`adsbLat`/`adsbLon`) keep working, unchanged, and remain the storage keys. New keys: `stationAltM`,
  `stationAntM`, `stationLocSource`.
- A location from the environment never overwrites one the user saved.
- Coordinates are validated: latitude −90..90, longitude −180..180, both finite. Altitudes are −500..9000 m.
- User-visible strings use `tr()`/`N_()`. Commits are made as AxolDad, with no attribution lines.

## Review Focus

1. **Existing users:** a user with `adsbLat`/`adsbLon` already saved sees the same position everywhere after the update.
2. **Malformed input:** malformed pasted text gives a clear one-line error and changes nothing. That covers a locator of the wrong
   length, a link with no coordinates, "52,1 -1,3" with decimal commas, and swapped lat/lon outside the valid range.
3. **No location set:** maps still open, at the old defaults, with a "Set your location" hint. Range calculations show "—" rather than
   distances from 0,0.
4. **GPS without a fix:** a receiver that sends sentences but has no fix does not change the location, and says "no fix yet".
5. **Remote input:** a remote station's position from its status is range-checked before the Stations window or CrossFixer uses it.

---

### Task 1: Core: StationLocation, parsers, NMEA/gpsd

**Files:** create `core/include/dect2/geo/station_location.h` and `core/src/geo/station_location.cpp`; test
`tests/geo/test_station_location.cpp` (register `station_location`, link dect2core).

**Interfaces:**
- `struct StationPos { double lat = NAN, lon = NAN, altM = NAN, antM = NAN; std::string source; /* "manual", "map", "gps", "locator", "env", "legacy" */ bool valid() const; };`
- `bool parseLocationText(const std::string& text, StationPos& out, std::string& err)`. It accepts:
  - decimal pairs: `"52.1, -1.3"`, `"52.1 -1.3"`, `"52.1N 1.3W"`;
  - degree–minute–second pairs;
  - Maidenhead locators of 4, 6 or 8 characters (the centre of the square);
  - Google Maps links: `@lat,lon,` and `?q=lat,lon`;
  - OpenStreetMap links: `#map=z/lat/lon` and `mlat=..&mlon=..`;
  - geo URIs: `geo:lat,lon`.
  It reuses `fromMaidenhead` and `parseCoordinates` where they fit.
- `bool validPos(double lat, double lon)` and `bool validAlt(double m)`.
- `struct GpsFix { bool fix = false; double lat, lon, altM; int sats = 0; };`
- `bool gpsFromNmea(const std::string& line, GpsFix& io)`: GGA and RMC; it uses `rfi::parseNmea` if that suits.
- `class GpsdClient { bool start(host = "127.0.0.1", port = 2947); void stop(); bool latest(GpsFix&) const; std::string status() const; };`
  - It sends `?WATCH={"enable":true,"json":true}` and reads TPV.
  - It runs on its own thread and reconnects with backoff.
- `StationPos envLocation()`: reads `SAURCOM_LOCATOR`, or `SAURCOM_LAT` + `SAURCOM_LON` (+ `SAURCOM_ALT`).

- [ ] **Tests:** every accepted format gives the right numbers (within 1e-6, or the square's centre for locators); each malformed case in
  Review Focus 2 gives an error; NMEA GGA with a fix and without one; RMC with status V is ignored; a gpsd TPV line parses (feed the
  parser directly, without a socket); env parsing.
- [ ] Implement, run `ctest -R station_location`, and commit: `location: one station position, parsers, GPS (NMEA/gpsd)`.

### Task 2: App: the single setting, the Settings page and the toolbar chip

**Files:**
- `app/adsb/adsb_ui.cpp:383-389`: `receiverLocation`, `receiverLocationSet` and `setReceiverLocation`. Move these to a new
  `app/state/station_loc.cpp`/`.h`, keep them as the single source, and add altitude and antenna height.
- A new **Settings → My station** page: find where the settings pages live in `app/ui/`.
- The toolbar location chip: `app/ui/toolbar.cpp`. It shows "📍 52.10, -1.30" or "Set location", and opens the page.
- `app/ui/wizard.cpp` (the location step uses `parseLocationText`).
- The ADS-B screen's location box (`adsb_ui.cpp:~91`): it uses the same page. Keep **Locate (Wi-Fi)** on macOS.
- Startup: if no position is saved and `envLocation()` is valid, use it with source "env" and do not save it.

- [ ] The page has:
  - a paste box using `parseLocationText`, with a preview of the result;
  - latitude, longitude, height and antenna height fields;
  - a **Use GPS** section: serial NMEA, reusing the GPS reader in `app/panels/radio_tools_ui.cpp`, or gpsd at host:port;
  - **Use the fix once** and **Follow the GPS** (for mobile use);
  - the source of the current position.
- [ ] Compile `dect2`, then commit: `My station: one location page, toolbar chip, GPS, paste anything`.

### Task 3: Every map and range calculation uses it

**Files:** every screen that centres a map, or keeps its own position, or computes range/bearing:
- `grep` for `adsbLat`, `refLat`, `receiverLocation`, `map.lat`, and per-mode lat/lon prefs;
- known ones: ADS-B, AIS, APRS, sondes (`app/sonde/sonde_ui.cpp`), mesh (`app/mesh/mesh_ui.cpp:85`), DF (`app/df/df_ui.cpp`), HFDL
  (`app/hfdl/hfdl_ui.cpp`), the Eye map, satellite passes, propagation, NOTAMs (`app/adsb/notam_app.cpp:497`).

- [ ] Make each one read the station position, keeping its own value only when the user has deliberately moved that map. Open every map
  centred on the station.
- [ ] Add **Centre on me** to each map.
- [ ] Add a right-click **I'm here** on each map, which sets the station position with source "map" after a confirmation that shows the
  coordinates.
- [ ] Where no position is set, show "—" for range, and the hint "Set your location (toolbar 📍)".
- [ ] Fix the hint texts that point to "Tools > Map" or "the ADS-B screen" so they point to the toolbar chip.
- [ ] Compile `dect2`, then commit: `maps: open at my station, Centre on me, I'm here`.

### Task 4: Installer, container and station network

**Files:**
- `packaging/proxmox/saurcom-lxc.sh`:
  - `--location TEXT` (decimal pair or locator);
  - if not given and the run is interactive, ask: "Station location (lat,lon or Maidenhead locator, Enter to skip)";
  - validate the value with a small shell check;
  - pass `SAURCOM_LOCATION` into setup.
- `packaging/proxmox/saurcom-lxc-setup.sh`: write `SAURCOM_LOCATOR` or `SAURCOM_LAT`/`SAURCOM_LON` into `.env`.
- `packaging/docker/compose.yaml`: pass them through to the app.
- The image: check how env vars reach the app process (s6 init) and pass them through.
- Remote API status JSON: `server/server_host.cpp` and `app/platform/remote_api_app.cpp` add `"lat"`, `"lon"` and `"alt"` when set.
- `core/src/net/station_link.cpp` `readPosition`: range-check these values (Review Focus 5).
- The Stations window shows each site's position.

- [ ] Add a test in `tests/net/test_station_link.cpp`: out-of-range positions from a remote status are ignored.
- [ ] Add a dry-run check of the installer with `--location`.
- [ ] Run shellcheck. Commit: `location for containers and linked stations`.

### Task 5: Docs

**Files:** `docs/location.md` (new); `packaging/proxmox/README.md` (the `--location` option); `packaging/docker/README.md` (the env vars);
`README.md` (one line).

- [ ] Write it, run the fast suite, and commit: `docs: setting the station's location`.
