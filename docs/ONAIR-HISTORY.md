# OnAir release notes, 0.2.6 to 0.2.8

Eye of SaurCom is a fork of [OnAir](https://github.com/adswill/OnAir). These are OnAir's own release notes for the versions the fork grew from, kept for reference. The program name in them has been changed along with the rest of the code; the work described is OnAir's.

## OnAir 0.2.8

<details>
<summary><b>Show the 0.2.8 changes</b></summary>

- **22 new modes** (142 in all): the **KA family** (KAAL, KAPP, KAFT, KAVB, KAIT, KAWS, see below), eLoran / Loran-C, MPT1327, LTR and EDACS trunking control channels, Yaesu Fusion, a military HF waveform identifier (identify only), ARDOP, GOES DCS, Mode A/C, a BLE tracker detector, AM stereo (C-QUAM), DGPS beacons, SITOR, DME, FSQ and IFKP. Several are marked *in development* in the app and here: they have only been tested on generated signals.
- **Voice you install yourself:** TETRA voice (unencrypted calls only) and Yaesu Fusion voice through a user-installed codec plug-in, for example one built from [mbelib-neo](https://github.com/arancormonk/mbelib-neo). Nothing patented is bundled; see [docs/CODECS.md](docs/CODECS.md).
- **Easier to use:** a command palette (Ctrl+K), a get-me-listening wizard, four guided tours, workspaces with crash restore, a recording library with re-analyse, a Report a problem dialog with a local diagnostics bundle, a typography choice (Current, Clean, Compact) and a larger-touch-targets option.
- **Receiver and display:** the demod mode shown and settable in the FFT panel, with a name search in the mode list (#212); waterfall export as PNG frames and a timelapse; a Solar and Jupiter burst monitor; FM tone control and a warm preset; C-QUAM stereo; VHF DSC channel 70; a toolbar button that says Memory; contact lines between stations on the FT8 and JS8 maps.
- **Satellite dishes:** a dish alignment assistant (beacon level meter and look angles), LNB calibration from a beacon, a rotator north-offset calibration, a LEO Doppler monitor (detect only) and a SatNOGS transmitter browser (in development).
- **Synchronous AM and the co-channel canceller:** the General receiver has a SAM demodulator next to AM. A loop locks the station's own carrier instead of measuring the envelope, so fading that takes the carrier down no longer distorts the audio, and either sideband can be heard on its own to step around a station crowding the other side. For two AM stations on one channel, switch on Co-channel: it locks the near station, rebuilds it from the sideband the far one is not on and subtracts it, so the station underneath comes up out of it. Slide the depth to 0 to hear the channel untouched and compare. Receive only.
- **Radio tools:** Tools > Radio tools gathers the receive-only tools in one window, grouped as a pocket multi-tool would: Wi-Fi, Bluetooth, sub-GHz and spectrum, logging. It adds a deauthentication flood alarm, an evil twin check (the same network name open on one address and protected on another) and a list of Bluetooth serial module names (HC-05 type, used by some card skimmers), all read from what the Wi-Fi and Bluetooth modes already hear; a frequency analyzer (the strongest carriers in the span with a Tune button, and sub-GHz band presets); and a drive log that writes the networks (and, if you ask, Bluetooth devices) heard with a GPS or receiver position to a WiGLE CSV or KML file on your computer. Nothing in it transmits, joins a network, pairs with a device or captures a key exchange.
- **Deep Space Network:** Tools > Deep Space Network shows what NASA's dishes at Goldstone, Madrid and Canberra are talking to now (the public DSN Now feed, every 5 s): spacecraft, distance and light time, up and down links with band, data rate and power, the band plan of each link, and a Tune button when the feed publishes a downlink frequency.
- **Radios:** DXpatrol SDR (RTL2832U based, HF direct sampling, #220); PlutoSDR no longer contacts network Plutos you did not add (#215); the Linux RX888 start and a failed LimeSDR probe now name the device (see the release notes).
- **Fixes:** GOES-19 HRIT differentially coded (NRZ-M) frames decode, HD Radio long station names and per-service logos, HaLow 2 MHz sub-channel scan and vendor elements (#192, #167), LTE / NR scan filters (#164), ADS-B altitude colours and display options, spectrum analyser resolution, DVB-T2 HEVC software fallback (#203), Android USB (#201), Meteor-M Doppler retune (#199), a DAB crash (#210), CDR audio recovery (#138, #121), a mesh clock that keeps running through a receiver reset (PR #217, thanks @ffishman).
- **DVB-S / S2:** carrier survey, symbol dump and DVB-RCS / DVB-CID work from the DecSAT code by Arclamp (GPL-3.0-or-later), in development.
- The full list, with issue numbers and known limits, is in the release notes.

### The KA family (own modes, in development)

KAAL, KAPP, KAFT, KAVB, KAIT and KAWS are digital modes designed from scratch by Kristijonas Adomaitis for amateur radio and built into Eye of SaurCom. Every transmission carries the sender's callsign, nothing is encrypted, and they use their own unpatented codecs. **All figures below were measured in simulation (generated signals with simulated noise); no KA mode has been on the air yet.**

| Mode | Job | Width | Notes |
|---|---|---|---|
| KAAL | voice, beacon, station ID | about 10 kHz (8 ksym/s, 16-QAM) | own 12 / 16 kbit/s voice codec live; 24 / 32 kbit/s live at 16 ksym/s; the callsign is also hidden in a sweep signature |
| KAPP | short messages and small data | 100 kHz | nine profiles, BPSK to 1024-QAM, selective-repeat bursts |
| KAFT | file transfer | 200 / 500 / 1000 kHz | ten profiles, hash-checked files |
| KAVB | video | 1 to 2 MHz | physical layer and container only; no video codec yet (in development) |
| KAIT | still pictures | under 200 kHz | a picture in a 10 s burst, sent in layers |
| KAWS | HF weak-signal chat and beacons | 2.4 kHz | chirp and OFDM profiles in UTC slots |

KAAL, KAPP and KAFT can write an IQ file you can open in SDR++ or Eye of SaurCom without a radio. KAPP and KAFT only listen: they send no acknowledgement.

</details>

## OnAir 0.2.7

<details>
<summary><b>Show the 0.2.7 changes</b></summary>

- **About twenty new modes** (120 in all): Wireless M-Bus, tone decoders, COSPAS-SARSAT 406 MHz, MT63, Z-Wave, meteor scatter, analog FPV video, Wi-Fi HaLow, a P25 trunking follower, e-Czas, a working LBJ decoder, and KiwiSDR as a network radio.
- **New tools:** [band survey](docs/band-survey.md), a [message database](docs/messages.md) with full-text search, a [flight history](docs/flight-history.md) with per-aircraft dossiers, [watchlists](docs/watchlists.md) with ntfy / Telegram / webhook alerts, a [time machine](docs/time-machine.md) (a rolling memory of raw IQ), a pass scheduler, a [Doppler orbit fit](docs/doppler-orbit.md), [self-calibration](docs/self-calibration.md), [station health](docs/STATION_HEALTH.md), an [interference hunter](docs/rfi-hunter.md), HF propagation and GNSS navigation views, a [hydrogen line toolkit](docs/hydrogen-line.md), [passive radar](docs/passive-radar.md), an [RF camera](docs/rf-camera.md) and a [protocol lab](docs/protocol-lab.md).
- **TV and DAB:** a [TV recorder](docs/tv-recorder.md) (programme guide grid, scheduled recording, timeshift, subtitle export) and a DAB programme guide with XMLTV export.
- **Plugin SDK** ([docs/plugins.md](docs/plugins.md)): add a mode or action as a plugin file without rebuilding. **Headless server** ([docs/headless-server.md](docs/headless-server.md)): `saurcom-server` runs Eye of SaurCom without a window, controlled from a browser. **Raspberry Pi** ([docs/RASPBERRY_PI.md](docs/RASPBERRY_PI.md)): arm64 install and which modes fit (untested on real hardware).
- **Audio and signals:** an [audio cleanup chain](docs/audio-cleanup.md), [auto gain](docs/auto-gain.md) with per-device tables, live translation to English ([docs/TRANSCRIBE.md](docs/TRANSCRIBE.md)), auto-labelled spectrum, a calibrated signal identifier, a weather picture studio, [offline data packs](docs/PACKS.md) and [sound card input](docs/SOUND-CARD.md) for narrowband modes.
- **Interface:** a [touch interface](#features), full-screen maps, an airband airport search with an On the air tab, a redesigned Transmit panel, a new logo.
- **Android:** direct USB for RTL-SDR radios. The APK is signed with a new permanent key, so **uninstall 0.2.6 first**; later updates install normally.
- **Fixes:** DVB-T2 16K extended pilot table (the 642 MHz capture decodes), TETRA no longer calls clear cells encrypted, HFDL strong bursts, DRA+ bit rate, mode switch while running, rtl_tcp greeting wait, Airspy HF+ late callbacks, accented paths on Windows.
- **Under the hood:** the C++ protocol layers were replaced by Rust; **Rust is now required to build** (see [Build from source](#build-from-source)).
- Most new modes and tools have been tested on synthetic signals only; the full list is in the release notes.
</details>

## OnAir 0.2.6

<details>
<summary><b>Show the 0.2.6 changes</b></summary>

- **[Android app](#android-app):** the same decoder core with a phone interface (receive only).
- **Hamlib rig control and rotators:** drive CAT radios such as an Icom IC-R8600 or IC-7300 (frequency and mode follow in both directions), and aim an antenna rotator.
- **A real-time map of positions** (aircraft, ships, APRS, mesh nodes, radiosondes and more), with offline map files, and a receiver-range plot on the ADS-B and AIS maps.
- **DRM data services:** slide show, broadcast website, programme guide (EPG), TPEG and Journaline. DAB and DRM share the same MOT object handling.
- **Whole-span decoding** for pagers (POCSAG, FLEX) and APRS / packet: every channel in the captured span at once.
- **TV:** teletext pages and DVB subtitles, recording with pause and live buffer, multi-channel DVB-T/T2 from one wide capture, user-set channel logos, DVB-S2X VL-SNR frames, DTMB LDPC on the GPU.
- **ADS-B:** feeds from other receivers, offline registrations and countries, range statistics, metric vertical speed.
- **FM:** a Mono switch for weak stations, a saved-station library, a scan with full RDS.
- **Scroll to tune:** the wheel (or a trackpad) steps the frequency field digit by digit.
- **Share my radio:** an rtl_tcp server for the radio Eye of SaurCom is running.
- **More radios:** Fobos SDR, RX888 MkII (native USB 3 driver), RFNM, Red Pitaya, Pluto+ Ethernet, SpyServer, SDR++ server, Hermes Lite 2.
- **Transmit** of every mode that has a signal generator, listed in [docs/TRANSMIT.md](docs/TRANSMIT.md).
- **New modes** since 0.2.5: Tone decoders (CTCSS, DCS, DTMF, selcall), Wi-SUN FAN, e-Czas (Polish time service on 225 kHz), UAT 978 MHz, VDL Mode 4, TETRAPOL, Elektro-L, LBJ (Chinese railway warnings), COSPAS-SARSAT 406 MHz beacons, Wireless M-Bus, Z-Wave, MT63, FSQ, IFKP, meteor scatter, DECT, ANT+ sensors, NB-IoT and 5G NR cells, MAC, DVB-C, MeshCom, Q65 and the other WSJT modes, and more (120 modes in all).
- **Interface:** Light and High contrast themes, an accent colour, colour maps and scroll-back for the spectrum, a General receiver with up to 4 channels and MP3 / FLAC recording, a one-button **Record audio** in the Source panel (next to Record IQ) that saves the sound you hear in any mode to WAV, FLAC or MP3 (optionally only while there is sound, one file per transmission), **Monitor audio** (Source panel): hear the tuned channel of any mode, also the decoders that have no sound of their own, as USB / LSB / narrow FM / AM, on a chosen output device, a second device (a virtual audio cable for fldigi, WSJT-X, multimon-ng) or a local TCP stream (off by default, 127.0.0.1 only unless you allow other computers; see docs/AUDIO.md), a "Save bug report" zip, translations (German, French, Spanish, Chinese, Lithuanian, Hungarian, Japanese, Polish, Dutch, Portuguese, Italian, Russian).
- **Docker image** that shows the Eye of SaurCom GUI in a browser, published with each release.
- **mbelib is no longer included** (patented AMBE/IMBE vocoders): see [Good to know](#good-to-know).
</details>

