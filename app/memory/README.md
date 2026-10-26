# app/memory

The Memory panel: the frequencies a user keeps, with the mode and receiver settings they were stored with. The model, the files and the recall order are in core (`core/include/dect2/channels/memory_bank.h`, `core/src/channels/memory_*.cpp`); this folder is the receiver side and the screens.

## What you can do

- **Open it:** View > Memory..., `Ctrl+M`, or the **M** menu in the menu bar (the classic interface has an **M** button next to Start / Stop). View > "Show Memory as a tab" (in the **M** menu) adds a **Memory** tab to the main screen; it is off by default so the normal screen is unchanged.
- **Store current:** keeps the frequency, mode and sub-mode, sample rate, channel bandwidth, the gains of the radio and the mode's options. The name is prefilled with the station's own name when the mode reports one (FM: the RDS name), else "mode frequency". A memory on the same frequency and mode asks: update it, add another, or go back.
- **Recall** (double-click a row, the Recall button, or a line of the **M** menu): switches to the stored mode, applies its options, gains and sample rate, then tunes. A running receiver is stopped and started again when the mode or the sample rate changes, retuned live when only the frequency does; a stopped one is set up for its next start. **Frequency only** tunes and leaves the mode alone. A memory whose mode this build does not have says so and changes nothing.
- **Receivers:** with several receivers open a "Recall on" box picks RX1..RX4; by default it is the receiver on screen.
- **Find and order:** the search box matches name, notes, tags, group, mode, and a typed frequency (`162.4` finds 162.4000 and 162.4125; `162` finds the 162 MHz band). Sort from the box or by the table headings; drag a row to reorder in "Manual order" (no search, no groups). Favourites (the star) stay at the top; groups are folded sections.
- **Delete** has a 20-second **Undo delete**. **Edit** changes every field, including the options as `key=value` lines.
- **Import / export:** CSV and JSON (UTF-8). Import also reads a plain list of `frequency,name` lines (MHz; `kHz` / `Hz` units accepted; a bare number of 30000 or more is Hz) and an SDR++ Frequency Manager export (frequency, bandwidth, name and list; its mode numbers are not mapped). Duplicates are skipped.
- **Scan memories:** hands the memories marked "Include in the memory scan" (and that have a mode) to the Scanner, which tunes each one, switches the mode, listens for the dwell time and stops while the receiver reports a signal or lock. The Scanner's own list is kept aside and comes back with "Back to my list".

## Where it is kept

`memories.json` in the data folder (the same folder as the results; `DECT2_DATA_DIR` moves it). Saved after every change, by writing `memories.json.tmp` and renaming it over the file. A file that cannot be read at all is copied to `memories.json.corrupt` before anything can overwrite it; unknown fields and bad entries are ignored with one log line each on stderr. At most 10000 memories.

File format, version 1: `{"format":"saurcom-memories","version":1,"memories":[ {...} ]}`. Every field but `freqHz` is optional: `name`, `freqHz`, `mode` (the Scanner's mode id: a protocol id such as `pocsag`, the mode's own id, or `dvb atsc dab atsc3 isdbt fm`), `subId`, `family`, `stdMode`, `sampleRateHz` (0 Auto), `bandwidthHz`, `radio`, `gain` {`lna`,`vga`,`amp`,`db`}, `options` {key: value}, `group`, `tags` (comma separated), `notes`, `colour` (0xRRGGBB), `lastUsed`, `created` (unix time), `favourite`, `scan`, `dwellSec`.

CSV: a header row, then one memory per line, RFC 4180 quoting (commas, quotes and line breaks inside quotes), comma or semicolon delimited, columns `name, frequency_mhz, mode, sub_id, family, std, sample_rate_hz, bandwidth_hz, radio, lna, vga, amp, gain_db, group, tags, notes, colour, last_used, created, favourite, scan, dwell_s, options`. `options` is `key=value;key=value` with `%3B`, `%3D`, `%25` for those characters.

## Adding the options of a mode

A mode keeps its settings in `App` fields or in its own state. To have a memory carry them, register them where the mode is, with no other change:

```cpp
static MemoryOptionReg reg{{"myKey", [](const App& a) { return a.family == 99; },       // exists in this mode
                            [](const App& a) { return std::to_string(a.myValue); },     // read
                            [](App& a, const std::string& v) { a.myValue = atoi(v.c_str()); }}};   // put back (as the mode's own control does)
```

The built-in options are in `memory_options.cpp` (FM de-emphasis, mono, channel filter, RDS region; pager and packet width; DTMB and ISDB-T width; DVB-T/T2 width; analog TV); the general receiver registers its own in `general_ui.cpp`.

## Files

`memory_options.*` (capture, recall, the shared bank), `memory_ui.cpp` (window, tab, tables, the **M** menu), `memory_edit.cpp` (edit and duplicate dialogs), `memory_selftest.cpp` (`--memory-selftest`).

## Tests

`tests/channels/test_memory_bank.cpp` (the model, files, recall order), `tests/app/run_memory_recall.cmake` (a hidden run of a renamed copy of the program applies memories to a receiver and checks the state; macOS and Windows).
