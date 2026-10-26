# Eye_of_SaurCom: Bug Hunt Report

**Branch:** `claude/busy-curie-ef8t0e` (at `592665e`, same as fork `main`)
**Scope:** first-party code (`core/`, `app/`, `rust/`, `tests/`, `tools/`, `web/`, `packaging/`, `sdk/`, `server/`, `cmake/`, `.github/`, `CMakeLists.txt`, `data/`). `third_party/` excluded. Translation files, golden `*.log` data, lock files, and binary assets excluded.
**Method:** read-only review by parallel reviewers, one per shard. Each reviewer read its files in full and re-checked suspect code paths, grepping for definitions and callers. Lead reviewers then verified the highest-severity claims against the source.

## Coverage: read this first

This is a **partial** hunt, not a complete one.

- The reviewer subagents hit an API weekly usage limit partway through. Of 17 planned shards, **16 produced partial findings** and **1 produced none**:
  - **Shard 15 (`tests/` a–m, 721 files, ~91k lines):** not read line by line. A pattern scan covered every file, looking for duplicated `||`/`&&` operands, self-comparisons, constant `CHECK(true)`, and integer `abs()` on floating values. It found nothing reportable. The self-comparison hits are member-against-parameter lookups, and the four `CHECK(true, …)` calls deliberately check that a run survived without crashing. This is a weak pass: logic errors in expected values would not show up in it.
  - **Shard 09 (`core/include/dect2/` a–h):** completed, but most declaration-only headers were skimmed rather than read line by line.
  - **Rust decoder crates shard 12 never reached** (`saurcom_dmr`, `p25`, `fec`, `drm`, `vdl2`, `dvbsi`, `dab`, `wisun`, `ts`, `tetra`, `nxdn`, `hfdl`, `dstar`, `uat`; ~20k lines): a targeted pass by the lead reviewer, not a line-by-line read. It covered every raw-slice construction at the FFI boundary (`from_raw_parts`): each is guarded for null and zero length, or takes a documented fixed size. It also covered every slice bounded by a packet-derived length in the parsers: DMR text, DRM SDC, VDL2 XID, DAB JL and MOT, Wi-SUN MAC, and HFDL LPDU, each guarded before slicing. Neither turned up a defect. The VDL2 RS `n_parity` from C is clamped. This rules out the most likely panic and memory sites but does not cover decode logic.
  - **Shard 11 (`rust/saurcom_usb`):** completed in full; `cargo test -p saurcom_usb` passes (11/11).
  - **Shards 01–08, 10, 12–14, 16, 17:** partial. The reviewers got through some files and stopped before others, so some directories have no findings because they were never reached, not because they are clean.
- The brief asked for every file to be reviewed multiple times. That was not achieved. Each file in the reviewed portions got one full read, with a second pass only on risky files.
- **Treat "no finding" as "not checked," not "clean."** Directories with zero findings in the list below should be considered unreviewed.

Coverage gaps to close in a follow-up pass:
- `tests/` directories a–m (pattern scan only, no line-by-line read)
- Most of `core/src/` in shards 08 and 12 (remaining Rust crates)
- `core/include/dect2/` headers, declaration-only, not read line by line

## Summary

| Severity | Count |
|----------|-------|
| Critical | 2 |
| High | 6 |
| Medium | 25 |
| Low | 59 |
| **Total** | **92** |

Counts are as the shard reviewers rated them. One item (M25) was re-rated from Low to Medium during the lead check, so the Medium list above has 26 entries.

Verification status:
- **Re-read against the source by the lead reviewer:** C1 (DMB PES underflow), H1 and M25 (rotator parser and sign handling), H2 (DMB division), C2 (T2 `p2Gather` code path), and M7 (prefs `clean()`).
- **Reported by the shard reviewer, not re-checked:** everything else. Confidence is as the reviewer assessed it. The thread-lifetime items in H6 are of this kind.

---

## Critical

### C1. DMB PES length underflow → out-of-bounds read / `length_error` (remote-reachable)
- **Location:** `core/src/dab/dab_dmb.cpp:487-492` (`DmbRemux::Impl::pesPacket`), reached from `feed()` at `:545-548`
- **Verified:** yes. `if (plen && 6 + plen < b.size()) n = 6 + plen - 9 - hl;` has no check that `6 + plen >= 9 + hl`, so `n` wraps to ~2^64 when `PES_packet_length` is small.
- **Failure:** a crafted PES packet in a DMB broadcast stream sends `n` huge. It is passed to `parseSl` and to `e.au.assign(p, p+n)`, so the code reads past the buffer or throws uncaught `std::length_error`, aborting the process. One TS packet is enough.
- **Fix:** require `6 + plen >= 9 + hl` before computing `n`; otherwise drop the packet.

### C2. DVB-T2 `p2Gather` writes/reads out of bounds using an unvalidated L1-pre `postSize` (nP2 > 1)
- **Location:** `core/src/t2/t2l1.cpp:515-526` (`p2Gather`), called from `core/src/t2/t2rx_l1.cpp:131,138` and `core/src/t2/t2rx_data.cpp:36,40`
- **Verified:** code path yes. `nPost` comes straight from `unpackL1Pre` (up to 262143) and reaches `p2Gather` before `decodeL1Post` checks it. The writes `stream[nPre+n+j*nP2]` and reads `sym[n][idx]` are unbounded against `nP2*cP2` and `cP2`.
- **Caveat:** the reviewer requires an L1-pre whose CRC-32 and LDPC both pass, so exploitation needs a crafted or very lucky signal. Severity is Critical for the memory-safety impact, but practical reachability is lower than the rating suggests.
- **Fix:** reject the L1-pre when `1840 + postSize > nP2*cP2` before calling `p2Gather`, and bounds-check both indices in `p2Gather`.

---

## High

### H1. Rotator reply parser can throw `std::invalid_argument` on a bare `"."`; uncaught on a worker thread → `std::terminate`
- **Location:** `core/src/source/rotator.cpp:117,127,138,145` (called from `pollPosition` at `:395`, worker thread at `:540`)
- **Verified:** yes. The unsigned branch accepts a digit-or-dot run, so `"."` reaches `std::stod`, which throws. No `try`/`catch` around `run()`.
- **Failure:** any rotator (serial or TCP peer) answering the poll with `"OK."` or `"..."` terminates the app. Over TCP, any host on the port can trigger it.
- **Fix:** use `strtod` with an end-pointer check and require at least one digit, or wrap the parser in `try`/`catch`.

### H2. DMB audio access unit: integer division by zero when ASC gives `rate == 1` with SBR
- **Location:** `core/src/dab/dab_dmb.cpp:293` (`audioAu`), with `parseAsc` at `:134-152`
- **Verified:** the expression is as reported. `asc.rate/2` is 0 when `rate == 1`, so the division is by zero (SIGFPE). It runs even when audio output is disabled.
- **Fix:** compute the core rate first and fall back to 1920 when it is ≤ 0. Clamp `parseAsc` rates to a sane range.

### H3. DRM equaliser reads the symbol ring at a negative index right after frame sync
- **Location:** `core/src/drm/drm_rx.cpp:634-637` (`recIf`), called from `equalize()` (`:816-822`), started by `frameSyncStep` (`:722-731`)
- **Verified:** the index pattern is as described. `recIf` uses `ring[(size_t)(idx % kRing)]` with a signed `idx`, and `equalize` has no `t >= gy` guard that `freqLoop` has. The reviewer's confidence is medium.
- **Failure:** when sync is found early, `h0 < gy`, so `recIf(h0 - gy)` gets a negative index. `idx % kRing` is negative and casts to a huge `size_t`, reading before the ring buffer.
- **Fix:** return `nullptr` from `recIf` when `idx < 0`.

### H4. DRM SDC decoder reuses a stale `MlcCode` after an occupancy change → heap over-read
- **Location:** `core/src/drm/drm_dec.cpp:146-156` (`decodeSdc`), with `:88-92` (`decodeFac`) and `:23` (`reset`)
- **Reviewer confidence:** medium.
- **Failure:** the broadcaster changes spectrum occupancy while the SDC mode is unchanged. `layout_->nSdc` changes and `sdcZ_`/`sdcW_` are resized, but the old `MlcCode` is kept and decodes the new buffers. It reads `cells()` entries from the old size, a heap over-read.
- **Fix:** rebuild `sdcCode_` when `nSdc` changes, or reset it in the occupancy branch of `decodeFac`.

### H5. UAT receiver mutates the tracker from the DSP thread without the mutex `telemetry()` uses → iterator invalidation
- **Location:** `core/src/uat/uat_rx.cpp:27-35` (sink), `:60-62` (`feed`), `:80-94` (`telemetry`)
- **Reviewer confidence:** medium.
- **Failure:** `telemetry()` iterates `ac_`, `log_` and `fisb_` under `mu`, while the sink modifies them unlocked. Concurrent UI polling during a decode can crash or return garbage.
- **Fix:** take `p->mu` around `tracker.apply` and around demod `configure`/`reset`.

### H6. Gallery / watchlist / automation / specan / mesh / RF-cam threads are joinable at exit → `std::terminate` on every normal quit
- **Location:** several, grouped. Joinable `std::thread` objects that nothing joins or detaches before static destruction:
  - `app/panels/automation_sink.cpp:44,226` (`gHookTh`, webhook worker)
  - `app/watchlist/watchlist_ui.cpp:47,50-63,78` (`gTh`, watchlist push worker)
  - `app/specan/specan_ui.cpp:49,89,101` (`State::autoTh`)
  - `app/mesh/mesh_internal.h:47` and `app/mesh/mesh_settings_ui.cpp:99-111,130` (`mcThread`)
  - `app/df/rfcam_ui.cpp:47,135-144` (`th`)
- **Reported by the shard reviewer, not re-checked.** The reviewers report that each worker loops on a condition variable with no stop flag, so the join never happens and destruction aborts. Confirm before fixing.
- **Impact:** after one of these features is used, the process aborts on exit. The crash handler then reports "last run crashed" on the next start.
- **Fix:** add a stop flag and join from `shutdownApp`, or detach at creation.

---

## Medium

### Security and input handling

- **M1. Remote-control command 0x0e turns the bias-tee (antenna DC power) on for any LAN client.** `app/panels/feeds_ui.cpp:86`. The UI promises only "frequency and gain". Any host can enable bias-tee and retune, with no confirmation. The remote gain is also applied unclamped. *Fix:* drop the 0x0e case or gate it behind a separate setting; clamp remote gain and frequency.
- **M2. Unbounded detached threads in NetTuner; `stop()` can return while connection threads still use freed memory.** `core/src/net/nettuner.cpp:325-333, 205-212, 361-371`. Every accepted socket gets a detached thread with no cap and no `SO_RCVTIMEO`. `stop()` waits only 4 s, then `delete p_` frees state that stalled threads still reference. *Fix:* set receive timeouts, cap connections, and join rather than detach.
- **M3. NetTuner key check uses substring matching.** `core/src/net/nettuner.cpp:194-197`. `query.find("key=" + key)` accepts any query containing that substring, so a short key matches longer keys with it as a prefix. *Fix:* parse the parameter and compare for equality.
- **M4. Key-PIN rate limit is per room, letting anyone lock the real host out of moderation.** `web/rooms/src/room.js:41,48-52`. Five wrong PINs per minute, from any client, lock the legitimate host out. *Fix:* rate-limit per client IP hash.
- **M5. Map page loads MapLibre from unpkg without Subresource Integrity, and ships no CSP.** `web/map/public/index.html:290,365`. A CDN compromise would run arbitrary script. *Fix:* add `integrity` and `crossorigin`, or self-host, and add a CSP.
- **M6. Malformed percent-encoding in the map URL hash throws at module load and stops the map app.** `web/map/public/app.js:768-775`. `decodeURIComponent` is unguarded and called at top level. A shared link with a bad escape leaves the page on "Connecting". *Fix:* try/catch and return null.

### Data corruption and wrong output

- **M7. Preferences stripping destroys multi-line and pipe-delimited values on Linux and Windows.** `app/platform/platform_posix.cpp:395,433`. Verified. `setS` replaces `\n`, `\r` and `|` with spaces, but several callers store values that rely on those characters: FM station lists (`fmStations`), scan lists, selfcal history, satellite lists (`passCubes`, `cubesatSats`), mesh channel keys (`meshKeys`), passive radar, ADS-B sources, and TX presets. Saved data is silently corrupted or dropped after restart. The macOS Cocoa prefs path is not affected. *Fix:* escape and unescape reversibly, or use a separator-free encoding.
- **M8. Map upload marks ACARS and voice messages "sent" before the rate limiter, so most are never posted.** `app/map/map_send.cpp:967-976` (`attachMsgs` 510-533, `attachVoice` 537-559). `sent[]` is updated before the 4-second hold-off check, so messages arriving during the hold-off are recorded as sent but never posted. *Fix:* check the rate limit before attaching, or commit `sent[]` only after `startPost` accepts.
- **M9. H-line session loader trusts grid dimensions from the file.** `app/astro/hline_ui.cpp:278-308`, `core/src/astro/hline_io.cpp:225-229`, `core/include/dect2/astro/hline.h:107`. A `vstep` of 0 or a short row causes a division by zero or an over-read on the next frame. `spec 0 99999999999999` throws out of a UI callback. *Fix:* validate `vstep > 0`, `vhi > vlo`, and `1 ≤ nv ≤ limit`; cap `n`.
- **M10. Rate-limited spot batching in PskReporter holds the spot mutex during DNS.** `core/src/net/pskreporter.cpp:185-200` and `172-184`. A stalled resolver blocks `spot()` from the decoder threads, stalling FT8 and WSPR decoding. *Fix:* resolve outside the lock.
- **M11. Rotator connect blocks the caller for the OS timeout.** `core/src/source/rotator.cpp:287-308, 339-346, 527-541`. Plain blocking `connect()`, and `stopThread()` joins it, so `configure()` and `disconnect()` can hang about 2 minutes on an unreachable host. *Fix:* non-blocking connect with a short timeout.
- **M12. Windows file size via `ftell` (32-bit `long`) → files over 2 GiB reported as empty.** `core/src/wb/wb_source.cpp:244-260`. Also `geo/mbtiles.h:46-48,105` (the MBTiles reader, below). *Fix:* `_fseeki64`/`_ftelli64` or `std::filesystem::file_size`.
- **M13. MBTiles reader uses `long` for offsets, so raster maps over 2 GiB break on Windows.** `core/include/dect2/geo/mbtiles.h:46-48,105`. The `(long)` page offset wraps for large files. *Fix:* 64-bit seek and tell.
- **M14. WAV `fmt ` chunk length from the file sizes an allocation before any bound check.** `core/src/source/iq_file.cpp:89-92`. A length near `0xFFFFFFFF` allocates ~4 GiB and fails on open. *Fix:* cap the length, or check it against the remaining file size.
- **M15. Helper process frames can exceed the 4 MiB frame cap, killing a healthy plugin.** `core/src/plugin/proc.cpp:29,54` against `core/include/dect2/plugin/results.h:15` and `core/src/plugin/loader.cpp:125`. A 1024×1024 RGBA image is exactly 4 MiB plus a header, so it is rejected and the plugin is switched off. *Fix:* raise `kMaxFrame` above the largest payload, or cap audio sample count.
- **M16. Webhook preset bodies are not JSON-escaped in literal template text.** `core/src/automation/rules.cpp:71-108, 122-161`. Only substituted values are escaped. A template containing `"` or `\` produces invalid JSON, and the notification is silently lost. The final `resize` can also cut an escape sequence in half. *Fix:* escape the whole rendered string after truncating.
- **M17. Use-after-free window: raw `Runner*` used after the Manager mutex is released.** `core/src/plugin/manager.cpp:266-281, 428-435`. `stop()` destroys the slot while `settingChanged` or `feedEvent` may still be running on another thread. *Fix:* `shared_ptr` the runner and copy it out under the lock.
- **M18. Shared pointer fixed-table static init is not thread-safe.** `core/src/common/inflate.cpp:407-420`. Plain `bool init`, no `call_once`. Concurrent `inflateRaw` callers (zip install, PNG, updater) can see an uninitialised table. *Fix:* function-local static.
- **M26. Map client never removes expired tracks while the WebSocket is connected.** `web/map/public/app.js:1086-1091`, `web/map/src/index.js:537-561`. The hub expires tracks but never broadcasts removals, so ghost markers accumulate in long-open tabs. *Fix:* broadcast `remove` messages from the hub and handle them in the client.
- **M19. MeshCore builders write an inconsistent path length when the path has more than 63 hops.** `core/src/mesh/mesh_proto_meshcore.cpp:297-298, 313-314`. The length byte and the copied bytes disagree, producing a malformed frame. *Fix:* clamp to one `n` used for both.
- **M20. VDL4 sync-burst CPR decode uses a (0,0) reference, so positions are wrong away from the equator.** `core/src/vdl4/vdl4_proto.cpp:312` with `vdl4CprDecodeLocal` at `:163`. Reviewer-reported, not re-checked. The reviewer's reasoning: a 51.47°N aircraft decodes to ~3.47°N, and the `||` fallback is dead code because the function cannot return false for a (0,0) reference. *Fix:* use the receiver position or a real reference.
- **M21. CPR-decode check for the PLP mode switch uses non-atomic counters.** `core/include/dect2/t2/t2plp.h:69,90-94`. `setMode` writes `switched_`, `pathFrames_` and `slowRun_` while the decode thread modifies them. Only `mode_` and `autoGpu_` are atomic. *Fix:* make them atomic, or post a request that the decode thread applies.
- **M22. Unsynchronised log/frame callback invocations on the feed thread.** `core/src/wifi/wifi_rx.cpp:366,441`, `core/src/wisun/wisun_rx.cpp:736`, `core/src/halow/halow_rx.cpp:177`, and similar in `p25`, `nxdn`, `dstar`, `m17`, `dpmr`, `tetra`. The setters take the lock, but the feed thread invokes the stored `std::function` without it. Replacing a callback mid-decode is a data race. *Fix:* copy the callback under the mutex before invoking it.
- **M23. CSV/JSON memory import: out-of-range double → int conversions.** `core/src/channels/memory_io.cpp:186,188` (MHz/Hz columns skip the `< 1e13` check the JSON path enforces) and `:70,75,80`. Undefined behaviour on a hostile file. *Fix:* apply the same range checks to every branch.
- **M24. GPIO numbers ≥ 8 alias onto GPIO 0–7 in the RTL-SDR driver.** `rust/saurcom_usb/src/rtl.rs:201-215, 867-872` and `rust/saurcom_usb/src/ffi.rs:286-288`. `1u8.wrapping_shl(gpio)` masks to 0..7, so GPIO 8 clears bit 0, the bias-tee. This can switch off a forced bias-tee. *Fix:* return -1 for `gpio` outside 0..=7.
- **M25. Rotator sign handling double-negates negative values.** Shard 02 rated this Low, but it changes output on every negative reply, so it is listed here. Re-read and confirmed. `core/src/source/rotator.cpp:117-119, 138-139`. `stod` already keeps the sign, then the code negates again: `-010` elevation reads as +10, and `-030` azimuth as 30 rather than 330. *Fix:* parse the digits only, or drop the second negation.

---

## Low

Grouped by area. Each item lists location and a one-line description. See the shard files for full evidence.

### Core: source, net, cdr, channels, wb
- `core/src/source/hamlib_rig.cpp:39-56,69-90`: rig list strings are percent-decoded twice; `%` followed by hex is corrupted.
- `core/src/source/iq_file.cpp:69-70`: SigMF `sample_start × bytes-per-sample` can overflow `uint64`.
- `core/src/source/rotator.cpp:81-92`: `shortestPathAz` has no effect; all three candidates normalise to the same value.
- `core/src/net/ws_client.h:69-105`: a fragmented message is dropped if a receive timeout falls between fragments.
- `core/src/net/pskreporter.cpp:47-71,189-199`: one oversized spot silently drops the rest of the batch.
- `core/src/wb/wb_source.cpp:176-177`: `reserve()` sized from an untrusted container duration can throw.
- `core/src/wb/wb_measure.cpp:241-247`: a selection at the top edge gives an empty range and dereferences `end()`.
- `core/src/wb/wb_rx.cpp:18-38`: `feed()` writes shared state without the lock that `configure()`/`reset()` use.
- `core/src/channels/channel.cpp:96-99`: unsigned subtraction of monotonic counters wraps after a reset.
- `core/src/cdr/cdr_audio.cpp:75-81`: a DRA add-on returning success with bad frames can loop forever.

### Core: digital voice, sonde, mesh, uat, ADS-B, pager, GOES
- `core/src/p25/p25_rx.cpp:390`, `nxdn/nxdn_rx.cpp:389`, `dstar/dstar_rx.cpp:308,437,515`, `m17/m17_rx.cpp:428,536`, `dpmr/dpmr_rx.cpp:457`, `tetra/tetra_rx.cpp:347`: callback invoked without the mutex that guards its setter (see M22).
- `core/src/hfdig/hfdig_hamdrm.cpp:504,94`: `setLogCallback` assigns the `std::function` with no lock.
- `core/src/lbj/lbj_rx.cpp`: user log callback invoked while holding the non-recursive feed mutex; a callback that calls back into the receiver self-deadlocks.
- `core/src/mesh/mesh_proto.cpp:76`: a 32- or 64-digit hex PSK is always tried as base64 first, so hex is never reached.
- `core/src/uat/uat_demod.cpp:202,214,233,245`: frame timestamps are off by half the unconsumed buffer and mix sample-rate units.
- `core/src/adsb/adsb_demod.cpp:380-386`: a refined timing offset can index before the buffer start via `(size_t)x` on a negative `x`.
- `core/src/pager/pager_proto.cpp:125-171`: `flexPhaseBits` writes past its 88-word array when given 44 or more pages (latent; current callers pass few).
- `core/src/goes/goes_proto.cpp:220-236`: a hostile HRIT header can make every scan line allocate ~100 MB.

### Core: DVB, ATSC, DAB, HDR, WiFi, WiSun, HaLow
- `core/src/hdr/hdr_l2.cpp:156,215`: SIS message and slogan loops read past `msgHave[32]` / `sloganHave[16]` for large lengths.
- `core/src/wifi/wifi_rx.cpp`, `core/src/wisun/wisun_rx.cpp`, `core/src/halow/halow_rx.cpp`: callback races (see M22).

### Core: headers (`core/include/dect2/`)
- `core/include/dect2/common/queue.h:274-276`: `ReorderBuffer::reset()` does not reset the sequence counter; later items are held behind a gap.
- `core/include/dect2/geo/mbtiles.h:34`: `fopen` on a UTF-8 path; should use `dect2::u8fopen` on Windows.
- `core/include/dect2/geo/mbtiles.h:59-60,77-80`: `meta()`, `isOpen()`, and `hasIndex()` read shared state without the mutex.
- `core/include/dect2/geo/nav.h:118-135`: `DeadReckoner` keeps stale speed and course after a fix loss.
- `core/include/dect2/geo/nav.h:122-124`: longitude blend does not wrap across the antimeridian.
- `core/include/dect2/geo/nav.h:227-228`: `parseGpx` divides by zero when `maxPts == 0`.
- `core/include/dect2/common/rs_abi.h:272-274`: `rsAbiCheck` reads past the Rust size list when the lengths differ.
- `core/include/dect2/dect/dect_proto.h:137-142`: DECT RFPI fields may be shifted by one bit against the ARI layout. Unverified against a captured station; the test mirrors the same shifts, so it cannot catch it.
- `core/include/dect2/automation/event.h:180`: `Event::str()` can truncate in the middle of a UTF-8 sequence.
- `core/include/dect2/tv/tvjson.h:69-70`: a double is cast to `int64_t` before the range check (undefined behaviour), and NaN and Inf print as invalid JSON.
- `core/include/dect2/source/iq_record.h:31,41` and `core/src/source/iq_record.cpp:39`: writes SigMF `ci8_le` for CS8; SigMF 8-bit types have no endianness suffix.
- `core/include/dect2/uat/uat_gen.h:9-15`, `core/src/uat/uat_gen.cpp:72`: `UatTx.frame` is 48 bytes, but the uplink path reads 552. Unreachable today.

### Core: plugin, automation
- `core/src/plugin/loader.cpp:47,52`: a vtable `struct_size` check does not cover `process_iq`, which is then read past the struct.
- `core/src/plugin/manager.cpp:511-514`: install deletes the old plugin before the new one is moved in; a failed rename leaves no plugin.
- `core/src/plugin/proc.cpp:220-223,145-146`: the watchdog kills a pid without synchronisation, so a recycled PID could be signalled.
- `core/src/automation/rules.cpp:223-244`: numeric-looking text such as `10.0.0.1` becomes an invalid number literal, so the rule cannot be saved.
- `core/src/automation/rules.cpp:110-117`: the CSV formula guard prefixes negative numbers with `'`, corrupting numeric columns.

### App (`app/`)
- `app/panels/gallery_ui.cpp:785,142,765-767`: the deleted thumbnail's texture is still referenced by the same frame's draw list (medium in shard 13).
- `app/panels/gallery_ui.cpp:325-330`: uninitialised `swath` and `lpt` when a `.trk` file is truncated.
- `app/panels/msgdb_ui.cpp:90-100,190-191`: `gmtime_r` result unchecked, and the ImGui id buffer truncates past ten digits, so ids collide.
- `app/watchlist/watchlist_ui.cpp:47,50-63,78`: see H6.
- `app/platform/platform_posix.cpp:395,433`: see M7.

### Rust (`rust/`)
- `rust/saurcom_mesh/src/aprs.rs:41-61`: MeshCom `clean_text` accepts overlong, surrogate, and above-U+10FFFF UTF-8, contrary to its own doc comment.
- `rust/saurcom_usb/src/rtl.rs:349-353, 493-507`, `rust/saurcom_usb/src/r82xx.rs:308`: ppm correction of −1,000,000 or lower makes the crystal 0 and divides by zero in `r82_set_pll`. Aborts across FFI. *(Medium, shard 11.)*
- `rust/saurcom_usb/src/ffi.rs:317,326`: EEPROM `offset` truncated from `u32` to `u8`, so `0x1FA` writes to `0xFA`. *(Low, shard 11.)*
- `rust/saurcom_usb/src/r82xx.rs:308-313`: `nint` truncated to `u8` before the range check, so out-of-range PLL dividers are accepted. *(Low.)*
- `rust/saurcom_usb/src/rtl.rs:335-340`: unvalidated tuner crystal frequency; doubling `2^31` wraps to 0 and divides by zero. *(Low.)*
- `rust/saurcom_usb/src/e4k.rs:523-525`: E4000 IF stage and gain narrowed with `as` casts, so out-of-range values alias valid ones. *(Low.)*

### Tests (`tests/`)
- `tests/packet/test_packet_width.cpp:21,44,58-62`: the neighbour callsign is 16 characters, but AX.25 callsigns hold 6. The `foreign` check can never be true, and nothing asserts it. *(Medium.)*
- `tests/sonde/test_sonde_lms6.cpp:44-45`: the inverted-polarity iterations are built and then skipped by `continue`, so polarity handling is untested.
- `tests/sonde/test_sonde_m20_roundtrip.cpp:74-83`: the comment says all 120 year-month values, but `m += 5` exercises only months 1, 6, and 11.
- `tests/same/test_same_proto.cpp:62`: a duplicated, tautological operand in the epoch check.

### Web and tooling (`web/`, `data/`)
- `web/rooms/src/lobby.js:9-12,38-41`: live rooms vanish after a Durable Object restart, and heartbeats for unknown ids are ignored. *(Medium.)*
- `web/map/src/index.js:301-307`, `web/rooms/src/index.js:34-36,56-58`: a JSON `null` body causes an uncaught `TypeError` (500) instead of a 400. *(Low.)*
- `web/rooms/public/room.html`, `web/rooms/public/_headers`: an inline `style` is blocked by `style-src 'self'`. *(Low.)*
- `web/mirror-worker/worker.js:8`: `decodeURIComponent` unguarded; a malformed path causes a 500. *(Low.)*
- `web/map/public/types.js:9-10,13-14`: `BE40` is in both `NARROW` and `BIZJET`, so the Beechjet is always drawn as a narrow-body. *(Low.)*

---

## Recommended next steps

1. **Fix the two Critical items first** (C1, C2), then H1 and H2. All are reachable from RF or network input.
2. **Fix the thread-lifetime group (H6)**, since it causes an abort on every normal quit for users who enable those features.
3. **Fix M1** (remote bias-tee). It is a safety issue on user hardware.
4. **Fix M7** (prefs). It silently loses user data after restart.
5. **Run a follow-up pass** on the coverage gaps above, especially `tests/` a–m, the remaining Rust crates, and the declaration-only headers.
