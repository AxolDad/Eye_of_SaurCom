# Eye of ZorCom: design

One program for radio and communications, grown from Eye of ZorCom (GPL-3.0). New code is Rust; existing C++ is ported to Rust only when it is being changed anyway. No paid tiers and no gimmicks.

## Rules

- **Reuse first.** Eye of ZorCom already receives 142 modes (TV with a live player, broadcast radio, ADS-B, AIS, satellites, P25 with a trunking follower, LoRa, Meshtastic, MeshCore and more) and supports HackRF, RTL-SDR, Airspy, SDRplay, Pluto, LimeSDR, USRP and bladeRF. Build only the gaps.
- **Stay mergeable with upstream.** Rename only what the user sees. Internal identifiers (`dect2` binary, namespaces, crate names, the settings directory) stay, so Eye of ZorCom updates still merge cleanly and existing settings are kept.
- **Transmit is the operator's responsibility.** It is off by default. A one-time acknowledgement ("I will transmit only within my legal authority") turns it on. After that there are no band, call-sign, power or mode locks. A transmit indicator is always visible while on air.
- **Every step ships with tests** and keeps the existing suite (`ctest`, `cargo test`) green.

## Build order

| # | Step | Scope | Built on |
|---|---|---|---|
| 0 | Fork and rename | Visible name "Eye of ZorCom": window title, About, packaging names. | Eye of ZorCom |
| 1 | Transmit unlock | Replace the dummy-load-only rule with the acknowledgement above. | `core/src/tx` |
| 2 | Audio out and streaming | Any channel to any output device (virtual cables are OS devices: PipeWire, BlackHole, VB-Cable; we write no drivers). Icecast streaming. | Eye of ZorCom audio |
| 3 | ATAK / Cursor on Target | Publish tracked positions (aircraft, ships, APRS, Meshtastic nodes, sondes) as CoT over UDP/TCP; show received CoT on the map. New Rust crate. | Existing trackers |
| 4 | Mesh send | Meshtastic and MeshCore text and position out through the SDR. | Mesh receive, frame generator, step 1 |
| 5 | Trunking and scanning | Multi-system P25, DMR and NXDN trunking, talkgroup aliases, call recording, one scanner. sdrtrunk is the reference. | P25 follower, step 2 |
| 6 | Internet radio and video out | Station directory and player; re-stream decoded TV and FPV video. | Step 2, TV player |
| 7 | WiFi sensing (from RuView, MIT) | Presence, motion, breathing and heart rate per room from the CSI frames RuView's ESP32 firmware sends over UDP (port 5005). RuView's vital-sign algorithms are ported to the Rust crate `zorcom_wifisense`; a UDP listener runs one sensor per node; View > WiFi sensing shows them. People counting, pose and fall detection need RuView's trained models and stay out until they can run without a cloud service. | RuView |

## Out of scope

Kernel audio drivers (the OS already provides virtual cables), paid features, and accounts or cloud services of our own.
