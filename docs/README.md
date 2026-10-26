# Eye of ZorCom documentation index

The user guides and notes that live in the repository. The main README is [../README.md](../README.md); radios are in [../DEVICES.md](../DEVICES.md); the code layout is in [../ARCHITECTURE.md](../ARCHITECTURE.md).

| File | Topic | What it covers |
|---|---|---|
| [audio-cleanup.md](audio-cleanup.md) | Audio cleanup | A chain that cleans the sound you hear. |
| [auto-gain.md](auto-gain.md) | Auto gain (AGC) | The AGC keeps a radio's front end at the best operating point for the active mode. |
| [band-plans.md](band-plans.md) | Band plans | A band plan is the channel raster and the allocations of a country: |
| [band-survey.md](band-survey.md) | Band survey | A band survey keeps the sweeps of one frequency range over time, so you can see which signals are always there, which parts of the band are quiet, and what changed since  |
| [benchmark-modes-0.2.7.md](benchmark-modes-0.2.7.md) | Per-mode benchmark, 0.2.7 baseline | Made with `bench_modes --seconds 10 --rates both --out ...` (tests/bench/bench_modes.cpp) on a Mac under heavy load from other jobs, so only CPU time is used: |
| [codar-research.md](codar-research.md) | CODAR / HF ocean radar: what is publicly documented (research note, 2026-10-13) | Sources: |
| [CODECS.md](CODECS.md) | Voice and audio codecs | > **DMR, P25, NXDN, dPMR and D-STAR voice is silent until you install mbelib.** The calls, IDs and data are shown without it. |
| [credits-for-website.md](credits-for-website.md) | Credits and licences | Paste this block into the website and the wiki. |
| [datawell-research.md](datawell-research.md) | Datawell Waverider buoy HF link: what is public, what was found (research note, 2026-10-13) | Sources (all fetched and read; downloaded into a scratch directory, nothing executed): |
| [DECT.md](DECT.md) | DECT bands | Eye of ZorCom listens to DECT base-station beacons (identity, system information, bursts). |
| [doppler-orbit.md](doppler-orbit.md) | Orbit refinement from measured Doppler | The Passes window has an **Orbit from Doppler** section. |
| [eczas.md](eczas.md) | e-Czas (Poland, 225 kHz long wave) | e-Czas ("e-CzasPL") is the official time service of the Polish Central Office of Measures (GUM). |
| [fpv.md](fpv.md) | FPV video (analog) | The mode **FPV video (analog)** (TV group) receives the analog video links of drones and FPV goggles: |
| [frequency-db.md](frequency-db.md) | Frequency database | A built-in, searchable list of well-known frequencies for every mode (View > Frequencies, `Ctrl+Shift+F`, or the **Known frequencies** drop-down of the Tuner panel). |
| [gnss-map.md](gnss-map.md) | GNSS satellites on the map | Where the satellites are comes from the navigation message the receiver decodes (parity/CRC checked, complete and consistent sets only), computed at the |
| [gps-jamming-map.md](gps-jamming-map.md) | GPS interference map | ADS-B aircraft broadcast how good their own GNSS position is. |
| [headless-server.md](headless-server.md) | Headless server (zorcom-server) | `zorcom-server` is Eye of ZorCom without a window. |
| [RASPBERRY_PI.md](RASPBERRY_PI.md) | Eye of ZorCom on a Raspberry Pi | Pi 4/Pi 5 install, udev, headless use, which modes fit (untested on real hardware). |
| [hydrogen-line.md](hydrogen-line.md) | Hydrogen line toolkit (Radio astronomy mode) | Everything here works on the integrated spectrum of the Radio astronomy mode (21 cm, 1420.405751 MHz). |
| [kiwisdr.md](kiwisdr.md) | KiwiSDR (network HF receiver) | A KiwiSDR is a software defined receiver that covers 0 to 30 MHz and is reached over the internet or a local network. |
| [lbj.md](lbj.md) | LBJ (Chinese railway train approach warning) | Receive only. |
| [LICENSE-NOTICES-BINARIES.md](LICENSE-NOTICES-BINARIES.md) | Licences of the libraries inside the Eye of ZorCom packages | Eye of ZorCom is GPL-3.0-or-later (file LICENSE; source: |
| [messages.md](messages.md) | Messages: the decoded-message database | View > Messages opens one searchable store of everything the decoders heard: |
| [mscatter.md](mscatter.md) | Meteor scatter | The mode **Meteor scatter** (Weather & science group) counts meteors by radio. |
| [mt63.md](mt63.md) | MT63 | MT63 is the keyboard-to-keyboard mode of Pawel Jalocha (SP9VRC) that is built to survive fading, interference and a badly tuned dial: |
| [multi-sdr.md](multi-sdr.md) | Several radios at once (multi-SDR) | Warning shown when a receiver is added: |
| [noise-remover.md](noise-remover.md) | Neural noise remover (Listen) | Removes steady background noise from speech with RNNoise (Xiph.Org, BSD-3-Clause; vendored in |
| [nrsp-st.md](nrsp-st.md) | SDRplay nRSP-ST (network) | Source: |
| [optimising-a-mode.md](optimising-a-mode.md) | Optimising a mode | The rule: |
| [PACKS.md](PACKS.md) | Data packs (work without internet) | View > Data packs lists data you can download once and then use offline: |
| [pass-scheduler.md](pass-scheduler.md) | Pass scheduler | The Passes window (View > Passes) can take satellite passes by itself: |
| [passive-radar.md](passive-radar.md) | Passive radar (View > Passive radar) | Detects aircraft from the echoes of a transmitter that already exists (an FM or DVB-T station), with no transmitter of your own. |
| [PATHWAY.md](PATHWAY.md) | PATHWAY.md | New mode fpv (family 101, active 102, std 103): |
| [plugins.md](plugins.md) | Plugins | Plugins let other people add decoders ("modes") and actions to Eye of ZorCom without rebuilding it. |
| [position-estimation.md](position-estimation.md) | Aircraft position estimation | One estimator serves ADS-B, UAT, ACARS, VDL2 and HFDL (`core/include/dect2/track/`, `core/src/track/`, test `tests/track/test_track.cpp`). |
| [protocol-lab.md](protocol-lab.md) | Protocol lab | View > Protocol lab... |
| [rf-camera.md](rf-camera.md) | RF camera | The RF camera lays the direction finder's bearing spectrum over a picture as a heat map, so you see roughly where in a scene a signal comes from. |
| [rfi-hunter.md](rfi-hunter.md) | Interference hunter (View > Interference hunter) | Watches a band for new interference, guesses what kind of source it is, and helps you walk to it with a loud / quiet meter. |
| [same.md](same.md) | NOAA Weather Radio SAME (EAS alerts, US and Canada) | Receive only. |
| [sarsat.md](sarsat.md) | COSPAS-SARSAT 406 MHz distress beacons | Receive only. |
| [scanner.md](scanner.md) | Sweep scanner | A fast wideband sweep with a noise-floor measurement: |
| [self-calibration.md](self-calibration.md) | Receiver frequency self-calibration | A cheap radio's oscillator is off by tens of ppm: |
| [SOUND-CARD.md](SOUND-CARD.md) | Sound card input (audio device) | Pick "Sound card: |
| [STATION_HEALTH.md](STATION_HEALTH.md) | Station health | View > Station health. |
| [threading.md](threading.md) | Threading in Eye of ZorCom (0.2.7) | Survey of how threads are used, the shared framework (`core/include/dect2/common/threadpool.h`, `queue.h`, `stage_pipeline.h`), where it is applied and where it is delibe |
| [time-machine.md](time-machine.md) | Rewind (time machine) | The "Rewind" button in the diagnostics row keeps the last seconds of the radio's raw samples in memory, so the signal from just *before* |
| [tones.md](tones.md) | Tone decoders (CTCSS, DCS, DTMF, selcall, SELCAL) | The mode **Tone decoders (CTCSS, DCS, DTMF, selcall)** (digital voice and two-way radio group) listens to the audio of a narrow-FM channel, or to a sound card, and |
| [TRANSCRIBE.md](TRANSCRIBE.md) | Live transcripts: airband and marine voice as text | > **Off until you turn it on, and no speech model is included.** Eye of ZorCom downloads one only when you click, or reads a file you put in the models folder. |
| [TRANSMIT.md](TRANSMIT.md) | Transmit | **You are responsible.** Eye of ZorCom limits nothing: |
| [trunking.md](trunking.md) | Trunking follower (P25, DMR Tier III) | Receive only. |
| [tv-recorder.md](tv-recorder.md) | TV recorder: programme guide, recordings, timeshift, subtitles | Works on the DVB transport stream that Eye of ZorCom already decodes (DVB-T/T2, DVB-C, DVB-S/S2, ISDB-T, DTMB and the others that deliver an MPEG-TS). |
| [watchlists.md](watchlists.md) | Watchlists and alerts | View > Watchlists. |
| [wmbus.md](wmbus.md) | Wireless M-Bus (EN 13757-4) | The mode **Wireless M-Bus (meters)** (IoT & sensors group) lists the utility meters (water, heat, gas, electricity, heat cost allocators) that send their |
| [zwave.md](zwave.md) | Z-Wave (ITU-T G.9959) | A receive-only monitor for Z-Wave home automation networks on the sub-GHz bands. |
