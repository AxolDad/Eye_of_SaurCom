# Frequency database

A built-in, searchable list of well-known frequencies for every mode (View > Frequencies, `Ctrl+Shift+F`, or the **Known frequencies** drop-down of the Tuner panel). Click a row twice (or press **Tune**) and Eye of SaurCom switches to the entry's mode, applies the mode's tuning and tunes to the frequency; **Frequency only** keeps the mode; **Add to Memory** keeps the entry in the Memory panel. Entries can be starred, and you can add, import and export your own.

## What is in it

Calling channels, beacons, band plans and published satellite downlinks. **No station databases**: no radio or TV station lists, no repeater lists, no per-aircraft or per-ship data. Everything was compiled from public regulations and standards (the sources are below and in every entry's `source`), not from other programs' lists.

| Group (file in `data/frequencies/`) | Modes | Sources |
|---|---|---|
| `aviation.tsv` | ADS-B, Mode S uplink, UAT, FLARM, ACARS, VDL2, VDL4, HFDL, airband (guard, SAR, air-to-air, UNICOM), HF VOLMET, VOR, ILS, NDB, drone Remote ID, DJI | ICAO Annex 10, RTCA DO-260B / DO-282B, ETSI EN 301 842, ARINC 618 / 631 / 635, ASTM F3411 |
| `marine.tsv` | AIS, VDES, DSC (VHF and MF/HF), NAVTEX, weather fax, marine VHF channels 1-28 and 60-88 | ITU Radio Regulations Appendix 18, ITU-R M.1371 and M.493, IMO NAVTEX manual, DWD / NOAA / UKHO / JMA schedules |
| `hf.tsv` | FT8, FT4, FT2, WSPR, JT65, JT9, Q65, FST4, MSK144, JS8Call, PSK31, RTTY, CW, SSTV, FreeDV, HamDRM, Olivia and other keyboard modes, NCDXF beacons, ALE, time signals (WWV, WWVH, WWVB, DCF77, MSF, JJY, RBU, RTZ, BPC, ALS162, CHU, RWM, BPM), CODAR bands, Datawell, emergency centres of activity | WSJT-X and JS8Call default frequency tables, IARU band plans, NCDXF / IARU Beacon Project, national time services, ITU Resolution 612 |
| `qo100.tsv` | QO-100 narrowband presets for every mode used on the transponder (CW, FT8, FT4, JS8Call, PSK31, RTTY, WSPR, SSTV, FreeDV, packet, keyboard modes, SSB), wideband DATV for DVB-S/S2 | AMSAT-DL QO-100 narrowband band plan, Es'hail-2 transponder plan (docs/qo100-auto.md) |
| `amateur.tsv` | APRS by region, LoRa APRS, MeshCom, D-STAR, M17, PMR446 digital (DMR, dPMR, NXDN), VHF / UHF / microwave calling frequencies, ISS and amateur satellites, QO-100 | IARU Region 1 / 2 / 3 band plans, AMSAT, ARISS, Es'hail-2 transponder plan |
| `satellite.tsv` | NOAA APT, Meteor LRPT, HRPT / AHRPT, GOES HRIT / EMWIN, Elektro-L, GNSS signals (GPS, GLONASS, Galileo, BeiDou, QZSS, NavIC), Iridium, Globalstar, Orbcomm, Inmarsat-C and Aero, NOAA Weather Radio | operator interface control documents (IS-GPS-200, Galileo OS SIS ICD, BeiDou ICD, GLONASS ICD), NOAA / EUMETSAT / Roscosmos documents |
| `data.tsv` | ISM sensors, wM-Bus, LoRaWAN by region, Meshtastic and MeshCore defaults, POCSAG / FLEX paging by country, Wi-Fi 2.4 / 5 / 6 GHz channels, BLE, Zigbee, ANT+, HaLow, Wi-SUN, DECT, GSM-R, LBJ, TETRA / TETRAPOL, P25, cellular bands (metadata only), radiosondes, radio-astronomy lines | ETSI EN 300 220 / 300 328 / 300 175, FCC Part 15, IEEE 802.11 / 802.15, LoRa Alliance RP002, Meshtastic and MeshCore documentation, WMO, ITU footnote 5.340 |
| `pmr.tsv` | PMR446 analogue, FRS / GMRS, MURS, CB 27 MHz, LPD433 | ETSI EN 300 296, FCC 47 CFR Part 95, CEPT ERC/REC 70-03 |
| `broadcast.tsv` | DAB blocks 5A-13F, DVB-T/T2 VHF and UHF channel plans, DTMB, ATSC, ISDB-T channel centres, FM band edges (not stations), HD Radio, DRM | ETSI EN 300 401, ITU-R BT.470 and national channel plans |

Frequencies are facts about regulations and standards, so they are in the public domain; the text of the names and notes is written for Eye of SaurCom. Where a published plan has regional variants the entry says which region it is for (`EU`, `NA`, `US`, `UK`, `DE`, `R1` ...; `GLOBAL` shows in every region filter). Entries with an approximate or operator-dependent value say so in the notes; a few (Inmarsat-C and Aero channels, paging allocations, ALE channels) are "commonly used" values and should be checked against a current list before relying on them.

## File format

`data/frequencies/*.tsv` are compiled into the program by CMake (`freqdb_data.inc`), parsed once on first use by `core/src/channels/freq_db.cpp`. One entry per line:

```
freq_mhz|name|bandwidth_khz|notes[|mode|region|category]
```

Lines starting with `@` set the default for the lines below: `@mode`, `@region`, `@category`, `@source` (the source note kept in each entry). The last three fields override the defaults for one line. `mode` is the id a Memory uses (the Scanner's mode id: a protocol id such as `ft8`, `pocsag` or `meshtastic`, a mode's own id such as `adsb`, or `dvb` / `atsc` / `dab` / `atsc3` / `isdbt` / `fm`). `category` is one of `freqCategories()`. Keep each file under 16 KB (a string-literal limit of some compilers) and the total well under 400 KB. `tests/channels/test_freq_db.cpp` checks every file: known mode ids, no duplicate mode and frequency, each frequency inside its mode's band, categories, a source on every entry.

Rules for adding data: public regulations, standards and operator documents only; never copy another program's or website's list; one `@source` per group.

## Your own entries

*Add entry...* stores an entry in `frequencies.json` in the data folder (the folder of `memories.json`). *Import...* reads a CSV or JSON written by Export **or by the Memory panel**, or a plain `frequency,name` list (MHz); *Export CSV / JSON* writes your entries in the Memory file format, so a list moves between the two panels. Entries on a frequency and mode that exist already are skipped. Stars are kept in the settings (`freqFav`).

## QO-100

QO-100 entries give the RF downlink (10489.5 MHz band). The receiver works on the first IF behind the LNB, so tuning an entry subtracts the oscillator chosen on the QO-100 screen (9750 MHz by default). This holds for the `qo100.tsv` presets of other modes (FT8, SSTV, PSK31 ...) too: entries named "QO-100 ..." are tuned in the mode they name. The transponder does not invert; see docs/qo100-auto.md.
