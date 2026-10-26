# How Eye of SaurCom is put together

Eye of SaurCom is a C++20 program in two layers: a **receiver library** (`core/`) that knows nothing about windows, and a **graphical app** (`app/`) that draws what the library reports. The command-line tools in `tools/` use the same library.

## The path of a signal

```
radio ──► ring buffer ──► Engine ──► standard receiver ──► transport stream ──► Player ──► screen and speakers
(source)    (ring.h)    (analysis      T2 / DVB-T / ATSC / DAB     (ts.cpp)     (player.cpp)
                         thread)
```

1. **Sources** (`core/src/source*.cpp`) deliver complex samples from a radio or a file: HackRF natively, other radios through their own library (`source_native.cpp`) or SoapySDR (`source_soapy.cpp`), recordings and a built-in synthetic signal.
2. **Engine** (`engine.cpp`) owns the source, runs the analysis thread, feeds the spectrum analyser and the receiver that matches the chosen standard, watches the radio (unplug and reconnect), and publishes snapshots for the interface.
3. **Receivers**: DVB-T2 (`t2rx.cpp` finds the P1 preamble, tracks symbols, equalises; `t2l1.cpp` reads the signalling; `t2plp.cpp` runs the error correction), DVB-T (`dvbt_rx.cpp`), ATSC (`atsc_rx.cpp`), ATSC 3.0 (the `atsc3_*.cpp` files: `atsc3_sync.cpp` finds the bootstrap and cuts frames, `atsc3_frame.cpp` decodes a frame, then ALP, IP, ROUTE and `atsc3_remux.cpp` turn it into a transport stream), ISDB-T (`isdbt_rx.cpp` finds the mode and guard interval, tracks carrier and clock and reads the TMCC; `isdbt_demod.cpp` does the channel estimate, the interleavers, Viterbi and Reed-Solomon per layer; `isdbt.cpp` and `isdbt_tables_data.cpp` hold the carrier layout and TMCC coding, `isdbt_gen.cpp` is the transmitter used for the tests and `isdbtgen`), DAB (`dab_rx.cpp`), DVB-S/S2, DTMB, analog TV, DMR, DRM and ADS-B (one set of files per mode, `core/src/<mode>_*.cpp`, each with a test-signal generator; `modes.cpp` lists their tuning and test signals), FM radio (`fm_rx.cpp`: a channel filter brings the station to 500 kHz, a discriminator gives the multiplex, a pilot PLL drives the stereo decoder and the RDS decoder; `fm_gen.cpp` is the transmitter used for the tests). The error correction (LDPC, BCH, Viterbi, Reed-Solomon) lives in `ldpc.cpp`, `t2fec.cpp`, `dvbt_fec.cpp`, `dab_fec.cpp`. The LDPC decoder can run on the GPU (Metal in `gpu_ldpc.mm`, Direct3D 11 in `gpu_ldpc_d3d11.cpp`), with a self-test and the CPU as fallback.
4. **Transport stream** (`ts.cpp`, `bbunpack.cpp`): turns baseband frames into packets, parses the service list, guide and subtitles.
5. **Player** (`player.cpp`) decodes video and audio with FFmpeg, keeps the audio clock, and repairs short gaps (`conceal*.cpp`: Apple's model on macOS, a Direct3D 11 version on Windows, a CPU motion search elsewhere).
6. **Outputs** (`tsout.cpp`, `nettuner.cpp`): recording, UDP/RTP, and the built-in network tuner (HDHomeRun-style, M3U and XMLTV).

Threads: the analysis thread runs the receiver (it must never wait for the interface), the error-correction work runs on helper threads or the GPU, the player has its own reader thread and audio callback, and the interface runs on the main thread.

Every thread has a name (profilers, htop, Activity Monitor and crash reports show it): create threads with `dect2::namedThread("saurcom-xxx", fn, args...)` or call `dect2::setThreadName("saurcom-xxx")` first thing in the thread function (`core/include/dect2/common/thread_name.h`). Names are short and stable, `saurcom-<what>`, 15 characters at most (the Linux limit; longer ones are cut), with the receiver number appended where several run (`saurcom-rx2`).

## Folders

| Folder | What is in it |
|---|---|
| `core/include/dect2/<folder>/` | public headers, same folder names as `core/src` (`#include "dect2/<folder>/<name>.h"`) |
| `core/src/<mode>/` | one folder per decoder mode (DVB-T2 is `t2/`, AIS is `ais/` ...) |
| `core/src/engine/` | the engine thread, mode table and tuning, scanner, quality, wide-capture helpers |
| `core/src/source/` | radios and drivers (`source_*.cpp`), Hamlib, rotator, IQ files, gain |
| `core/src/audio/` | audio and video input/output, AC-4, encoders, shared ham audio front end |
| `core/src/transcribe/` | live transcripts: voice segmenter, speech-engine queue, callsign reader, transcript store, models (whisper.cpp in `third_party/whisper`) |
| `core/src/tv/` | transport stream, player, concealment, HLS, subtitles, teletext |
| `core/src/dsp/` | FFT and SIMD kernels, resamplers, LDPC (CPU, GPU), IQ correction, spectrum |
| `core/src/net/` | remote API, share server, network tuner and radio, data feeds, PSKReporter, WSPRnet |
| `core/src/geo/` | Maidenhead, satellite passes, map layers, tile cache, MBTiles |
| `core/src/channels/` | channel database and lookup |
| `core/src/bandplan/` | band plans: country channel rasters, CSV / JSON import |
| `core/src/results/` | results serialiser and telemetry JSON (the Android bridge reads these) |
| `core/src/tx/` | transmit |
| `core/src/common/` | ring buffer, XML, inflate, PNG, character sets, platform headers |
| `app/` | the window: `main.cpp` (loop and layout), `app.h` (state and shared declarations); `app/<mode>/` has a mode's screens; topical folders: `ui/`, `gfx/`, `panels/`, `map/`, `scan/`, `state/`, `platform/`, `i18n/` |
| `tools/` | `dect2cli` (receiver on the command line), `dect2scan` (channel scanner), `package/` (installer scripts), `dev/` (developer helpers) |
| `tests/<folder>/` | unit tests, benchmarks and a fuzzer, in the same folders as `core/src` (`tests/<mode>/test_<mode>_*.cpp` is picked up by a glob); `tests/app/` tests the window code, `tests/bench/` has the benchmarks; run with `ctest` |
| `packaging/` | icons, Windows installer script and toolchain file, Linux resources |
| `third_party/` | Dear ImGui and miniaudio, with their licences |

## Rust protocol layers

The frame, packet and file-format layers listed below are written in Rust (the workspace `rust/`, one static library `libsaurcom_rs.a`; `rust/README.md` is the guide) and the C++ they replaced is deleted: Rust is required to build (`cmake/SaurcomRust.cmake` stops with an error without cargo). The public C++ headers did not change, so callers do not know the difference; the functions of each header are implemented in the `<name>_rs.cpp` next to it, which calls the C ABI (`core/include/dect2/rs/<module>.h`). What is still C++ in those folders is receivers, demodulators, generators, trackers, key-dependent code and state (class stores, caches, locks).

| Module (crate) | C++ header and Rust-backed implementation | Still C++ in that folder |
|---|---|---|
| AIS (`saurcom_proto`) | `ais/ais_proto.h`, `ais_proto_rs.cpp` | generator side (`ais_proto.cpp`), receiver |
| ADS-B (`saurcom_adsb`) | `adsb/adsb_msg.h`, `adsb_msg_rs.cpp` | bit helpers (`adsb_msg.cpp`), generator, tracker, register |
| ACARS (`saurcom_acars`) | `acars/acars_proto.h`, `acars_proto_rs.cpp` | `acarsBuildFrame`, receiver |
| SAME (`saurcom_same`) | `same/same_proto.h`, `same_proto_rs.cpp` | county table and file loader |
| POCSAG / FLEX (`saurcom_pager`) | `pager/pager_proto.h`, `pager_proto_rs.cpp` | generators, POCSAG message assembler |
| AX.25 / APRS (`saurcom_ax25`) | `packet/packet_ax25.h`, `packet_aprs.h`, `packet_ax25_rs.cpp`, `packet_aprs_rs.cpp` | frame builder and transmit helpers |
| DMR (`saurcom_dmr`) | `dmr/dmr_fec.h`, `dmr_proto.h`, `dmr_fec_rs.cpp`, `dmr_proto_rs.cpp` | bit helpers, sync table, burst slicers, alias PDU builder |
| P25, NXDN, D-STAR (`saurcom_p25`, `saurcom_nxdn`, `saurcom_dstar`) | `<mode>/<mode>_proto.h`, `<mode>_proto_rs.cpp` | sync tables, receivers, generators, vocoder glue |
| TETRA (`saurcom_tetra`) | `tetra/tetra_pdu.h`, `tetra_pdu_rs.cpp` | encoders and generator PDUs |
| VDL2, HFDL (`saurcom_vdl2`, `saurcom_hfdl`) | `vdl2/vdl2_proto.h`, `hfdl/hfdl_proto.h`, `*_rs.cpp` | burst / PDU builders for the test signal, ACARS readers, demodulators |
| Mesh (`saurcom_mesh`) | `mesh/mesh_proto.h`, `mesh_pb.h`, `mesh_proto_rs.cpp`, `mesh_proto_lora_rs.cpp`, `mesh_pb_rs.cpp` | crypto, builders, settings tables, LoRa PHY |
| Wi-SUN (`saurcom_wisun`) | `wisun/wisun_proto.h`, `wisun_proto_rs.cpp` | channel plans, MPDU builder, receiver |
| UAT (`saurcom_uat`) | `uat/uat_decode_c.h`, `uat_decode_rs.cpp` | dump978 display functions (`uat_decode.c`), FEC, receiver |
| MPEG-TS (`saurcom_ts`) | `tv/ts_proto.h`, `ts_proto_rs.cpp` | demux state, EPG store (`ts.cpp`) |
| DVB service information (`saurcom_dvbsi`) | `common/text_charset.h`, `tv/dvb_text.h`, `teletext.h`, `dvbsub.h`, `*_rs.cpp` | decoder classes, iconv wrapper |
| DAB, DRM data (`saurcom_dab`, `saurcom_drm`) | `dab/dab_proto.h`, `drm/drm_proto.h`, `*_rs.cpp` | the decoder classes with their state |
| FEC (`saurcom_fec`) | `fec/fec_proto.h`, `fec_proto_rs.cpp` | the K = 7 Viterbi decoder (`fec_proto.cpp`): the C++ NEON / SSE2 / AVX2 one is the faster one |
| Files, network (`saurcom_files`, `saurcom_net`) | `files/files_proto.h`, `net/net_proto.h`, `*_rs.cpp` | file access, sockets, threads, state |

Two implementations stay on purpose, both opt-in with the C++ one as the default: the ring buffer (`-DDECT2_RUST_RING=ON`, `common/ring.h` keeps both) and the Android USB drivers (`-DDECT2_RUST_USB=ON`), plus the Rust NEON Viterbi kernel (`-DDECT2_RUST_VITERBI=ON`, about 4% slower than the C++ one). Each Rust-backed layer has a golden-vector test, `tests/<mode>/test_<mode>_proto_rs.cpp` (a digest of a seeded run, recorded from the C++ it replaced, helper `tests/common/golden.h`), and a seeded mutation run `fuzz_<mode>_proto` for the sanitizer builds.

## The app

`app/app.h` holds the `App` struct (all interface state) and the list of panels. `main.cpp` has the frame loop (the window and fonts are in `main_window.cpp`, start-up and the developer flags in `main_start.cpp`, the classic layout in `ui/draw_ui.cpp` and `ui/main_tabs.cpp`). The panels are grouped: `toolbar.cpp` with `toolbar_source.cpp`, `toolbar_rig.cpp`, `statusbar.cpp` (top bar, source options, the network and Hamlib rig windows, status bar), `mode_table.cpp` (the one-row-per-mode table) and `mode_select.cpp` (the mode selector), `ui_common.h` (helpers several screens share), `plots.cpp`, `plot.cpp` (our own plotting library on ImGui's draw lists: axes, lines, dots, bars, images, heat maps, panning and zooming), `analysis_tabs.cpp` (receiver analysis tabs), `tv.cpp` (player and guide), `scan_outputs.cpp`, `antenna.cpp`, `dab_ui.cpp`, `fm_ui.cpp`, `ui2.cpp`, `ui2_theme.cpp`, `ui2_widgets.cpp`, `ui2_shell.cpp`, `ui2_faces.cpp` (the new interface: palettes, small widgets, menu bar and top bar, and the Panel / Sidebar / Scope / Tiles layouts; the classic shell stays in `ui/draw_ui.cpp` and is one click away under View), `wizard.cpp` (first-run tour), and `widgets.cpp` (small shared pieces). Drawing goes through `gfx.h` (Metal on macOS, OpenGL elsewhere), and `platform.h` hides file dialogs and settings storage per operating system.

## Platform notes

- **macOS:** Accelerate (vDSP) for the signal processing, Metal for LDPC, CoreAudio, Apple's frame interpolation (macOS 15.4+, left out when building with an older SDK). Apple silicon uses NEON code; Intel builds use AVX2 and refuse to start on a processor without it.
- **Windows:** built for AVX2 (no run-time dispatch), Direct3D 11 for LDPC and picture repair, D3D11VA for video, miniaudio (WASAPI) for sound.
- **Linux:** the vector kernels exist twice (AVX2 and baseline) and the loader picks one; the GPU decoder is a stub, so error correction runs on the CPU.

## Tests

`ctest --test-dir build` runs the unit tests. Most use synthetic signals produced by the generators in the library (`t2gen.cpp`, `dvbt_gen.cpp`, `atsc_gen.cpp`), so they need no radio. `test_gpu_ldpc` compares the GPU decoder with the CPU one, `test_native` loads fake radio libraries, `test_conceal` checks the picture repair against known pictures. `test_fm_rx` sends a stereo station with RDS and noise through the FM receiver and checks the sound, the stereo separation and the decoded station name and text.

## Notes for outside builds

The Android bridge (`saurcom-android/native`, outside this repo) includes headers as `dect2/<name>.h`. Since 0.2.7 the headers live in folders: `dect2/remote_api.h` is `dect2/net/remote_api.h`, `dect2/modes.h` is `dect2/engine/modes.h`, `dect2/rx_json.h` is `dect2/results/rx_json.h`, `dect2/adsb_tel.h` is `dect2/adsb/adsb_tel.h`, and so on (mode headers go to their mode folder). `tools/dev/path_moves_0.2.7.txt` lists every old and new path. The bridge only needs `core/include` and the `dect2core` target, so its CMake does not change.
