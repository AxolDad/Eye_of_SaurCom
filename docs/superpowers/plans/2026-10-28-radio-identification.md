# Radio Identification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** It is easy to tell which physical radio is which when several are attached. The radio list shows the model, serial and USB port.
The user can name radios, find one by unplugging it, and give RTL-SDRs that share a serial a unique one. A radio that is plugged in is
announced.

**Architecture:**
- **Core (`core/include/dect2/source/radio_id.h`).** A radio gets an identity: kind + serial, or the USB port path when serials clash.
  - The identity comes with duplicate detection and a label store, `<data>/radios/names.json`.
  - A pure diff of two radio lists drives both "find by replugging" and "new radio" notices.
- **USB location.** Each `DeviceInfo` carries its USB port path (`usbPath`, e.g. `1-2.3`), wherever the enumeration can supply it.
- **The RTL-SDR serial rewrite** uses the existing `saurcom_usb_rtl_read_eeprom` / `saurcom_usb_rtl_write_eeprom`. It writes a backup of
  the whole EEPROM image first.
- **App.** The radio list, receiver tabs, the Eye, logs and `devicesJson` show the user's name.

**Tech Stack:** C++20, ImGui, the project's own USB layer (`core/include/dect2/rs/usb.h`, Rust `saurcom_usb`), `automation/json_mini.h`.

**Spec:** the design the user approved on 2026-10-28 in the conversation, quoted here:
1. **Tell radios apart in the list:** model · serial · USB port, plus `#1`/`#2` and a warning when two look identical.
2. **Find this radio:** unplug it, plug it back in, and it is highlighted. This works for every radio.
3. **Your own names:** tied to the serial, or to the USB port when serials clash. They are saved and shown everywhere: the radio list,
   receiver tabs, the Eye, logs and the web/remote API.
4. **Fix duplicate serials (RTL-SDR):** write a new serial to the EEPROM. It always asks first, warns that a replug is needed, and saves a
   backup of the original contents.
5. **Notice new radios:** "New radio: RTL-SDR (SN 1234). Name it / Use it in a new receiver tab." This also works in the Proxmox
   container, through the bound USB bus.

## Global Constraints

- C++20; no new third-party libraries.
- Cross-platform: Linux, macOS, Windows (MinGW-w64), and the Docker/Proxmox container. A USB path that a platform cannot supply is left
  empty, and the feature degrades: names fall back to the serial.
- Never open a radio another receiver tab is using (respect `core/src/engine/device_claims.cpp`).
- Polling for changes must be cheap: list radios at most every 2 s, never while a radio is streaming, and only through enumeration calls
  that do not open devices.
- EEPROM writes:
  - only on RTL2832U dongles;
  - only from an explicit user action with a confirmation dialog;
  - only after a full EEPROM backup file is written to `<data>/radios/eeprom-backup-<old serial>-<time>.bin`;
  - with a verify-by-read-back afterwards.
  - The new serial is 8 decimal digits and differs from every other attached radio's serial.
- User-visible strings use `tr()`/`N_()`. Commits are made as AxolDad, with no attribution lines.

## Review Focus

1. Two identical RTL-SDRs with the same serial: both are listed, labelled `#1`/`#2`, and a name given to one stays on that one, keyed by
   USB path, after a restart with both still in the same ports.
2. The radio a receiver is streaming from is never opened by the poller or the EEPROM tool. The EEPROM tool refuses with "in use by RX n".
3. An EEPROM write that fails halfway: the backup exists, the user is told how to restore it, and the restore action exists.
4. A label file that is corrupt, hand-edited or missing loads as empty, with an error note, and the app does not crash.
5. Name text from the user is shown as text, never as a format string. A name of 64 characters is cut.

---

### Task 1: Radio identity, duplicates, labels (core)

**Files:** create `core/include/dect2/source/radio_id.h` and `core/src/source/radio_id.cpp`; add `std::string usbPath` to `DeviceInfo`
(`core/include/dect2/source/source.h:122`); test `tests/source/test_radio_id.cpp` (register `radio_id`, link dect2core).

**Interfaces (produces):**
- `std::string radioKey(const DeviceInfo&)`: `"<kind>:<board>:sn:<serial>"`. When the serial is empty or shared, it is
  `"<kind>:<board>:usb:<usbPath>"`; when the path is unknown too, `"<kind>:<board>:name:<name>"`.
- `struct RadioRow { DeviceInfo dev; std::string key; std::string label; int dupIndex = 0; /* 0 = unique, else 1.. */ bool sharedSerial = false; };`
- `std::vector<RadioRow> identifyRadios(const std::vector<DeviceInfo>&, const RadioLabels&)`.
  - Shared serials are those seen on two or more radios of the same kind and board. Known factory defaults (`00000001`, `0`, empty)
    count as shared even when only one is attached.
- `class RadioLabels { bool load(path, err); bool save(path, err) const; /* file mode 0600, write to tmp then rename */ std::string get(key) const; void set(key, name); /* cut to 64 */ void remove(key); };`
- `std::string radioDisplayName(const RadioRow&)`: the label if set, else `"<name> · SN <serial> · USB <path>"`, leaving out empty parts,
  plus `" #n"` when it is a duplicate.
- `struct RadioDiff { std::vector<DeviceInfo> added, removed; }; RadioDiff diffRadios(const std::vector<DeviceInfo>& before, const std::vector<DeviceInfo>& after);`
  - Radios are matched by `radioKey`.

- [ ] **Tests:**
  - Two RTLs with serial `00000001` on paths `1-1` and `1-2`: keys are by usb, labels are `#1`/`#2`, `sharedSerial` is true.
  - One RTL with `00000001` alone: `sharedSerial` is true (factory default).
  - A labelled radio shows its label.
  - Labels round-trip through save/load (mode 0600 on POSIX). A corrupt file loads as empty, with an error.
  - A 100-character name is cut to 64.
  - `diffRadios` reports one added and one removed for a swap.
- [ ] **Implement, run `ctest -R radio_id`, commit:** `radios: identity, duplicates and user names`

### Task 2: USB port paths from the enumerations

**Files:** the native RTL, HackRF, Airspy (and other native libusb-based) listing code that `listNativeDevices` / `listHackrfDevices`
calls; the Rust `saurcom_usb` enumeration if that is where devices are listed.

- [ ] Fill `DeviceInfo::usbPath` as `"<bus>-<port>.<port>..."` (libusb `get_bus_number` + `get_port_numbers`) wherever the listing walks
  libusb devices without opening them. For Soapy devices, use a `usb`/`path` key when the driver offers one, and leave it empty otherwise.
- [ ] Add a test with a fake device list if the code allows; at the least, run a listing compile test. Commit:
  `radios: USB port path in the radio list`

### Task 3: The radio list shows identities; names everywhere

**Files:**
- `app/ui/toolbar.cpp` (the source dropdown, lines 25-66)
- `app/ui/listen_wizard.cpp:129`
- `app/ui/wizard.cpp` (the radio step)
- receiver tab titles (`app/state/receivers.cpp`)
- `app/panels/eye_ui.cpp`, if radios are named there
- `app/platform/remote_api_app.cpp` and `server/server_host.cpp` (`devicesJson` gains `label`, `serial`, `usb`, `dup`)
- the radio settings window (find where per-radio settings such as ppm and bias-tee are shown)

- [ ] Show `radioDisplayName` everywhere a radio name is shown to the user. Keep the internal name for matching saved workspaces
  (`app/state/workspace.cpp:131`) and `toolbar_rig.cpp:56,376`. Saved device names must still match: match by key first, then by name.
- [ ] Add a **Name this radio** action (a small pencil next to the selected radio, and in the radio settings). Labels live in
  `<data>/radios/names.json`.
- [ ] Show a warning row when serials are shared: "Two radios share serial 00000001: names follow the USB port. Give them unique serials
  (RTL-SDR) to keep names when they move ports." with a button that opens Task 5's tool.
- [ ] Compile `dect2` and `saurcom-server`. Commit: `radio list: model, serial, USB port and your names`

### Task 4: Find this radio, and new-radio notices

**Files:** create `app/state/radio_watch.cpp` (`void radioWatchTick(App&)`, called once per frame from the receiver tick path, not the
draw path); `app/ui/toolbar.cpp` (the **Find this radio** button and the highlight); notices through the existing toast/notify
mechanism.

- [ ] Poll the radio list at most every 2 s, using enumeration only (Global Constraints), and skip polling while the user has the
  dropdown open.
- [ ] **Find this radio:** a small modal says "Unplug the radio you want to find…", then "…now plug it back in". `diffRadios` over the
  polls identifies it. The modal says which entry it is, selects it and offers **Name it**. Cancel and a 60 s timeout are available.
- [ ] **New radio** notice (not shown in the first 5 s after start): "New radio: <display name>". Buttons: **Name it**, **Use in new
  receiver tab** (opens an RX tab with it), **Dismiss**. A removed radio that a receiver was using gives the existing "receiver
  dropped" path; no new notice.
- [ ] Put the pure logic (a state machine fed by lists and time) in core, with tests: unplug detected, replug detected, the wrong radio
  replugged, timeout. Commit: `radios: find a radio by replugging it; notice new radios`

### Task 5: RTL-SDR unique serial tool

**Files:** create `core/include/dect2/source/rtl_serial.h` and `core/src/source/rtl_serial.cpp`, using
`saurcom_usb_rtl_read_eeprom`/`write_eeprom` (`core/include/dect2/rs/usb.h:84-85`); app UI in the radio settings; test
`tests/source/test_rtl_serial.cpp` with a simulated EEPROM.

**Interfaces:**
- `bool rtlEepromParse(const std::vector<uint8_t>& img, RtlEepromInfo& out, std::string& err)`, using the standard RTL-SDR EEPROM layout
  (rtl_eeprom: header 0x28 0x32, vendor/product, string descriptors).
- `bool rtlEepromSetSerial(std::vector<uint8_t>& img, const std::string& serial, std::string& err)`: rewrites the serial string
  descriptor and keeps the manufacturer/product strings.
- `std::string rtlPickSerial(const std::vector<std::string>& taken, uint32_t seed)`: 8 digits, not in `taken`.
- `bool rtlWriteSerial(const DeviceInfo&, const std::string& newSerial, const std::string& backupDir, std::string& backupPath, std::string& err)`
  - It refuses a claimed radio.
  - It reads the full image, writes the backup file and checks the file's size.
  - It writes, reads back and compares.
- `bool rtlRestoreEeprom(const DeviceInfo&, const std::string& backupPath, std::string& err)`.

- [ ] **Tests:**
  - parse a real-layout image;
  - set a serial and parse it back;
  - the manufacturer and product are unchanged;
  - an image that is too small or has a bad header is refused;
  - picking a serial avoids the taken ones;
  - a simulated write failure leaves the backup and reports the restore hint.
- [ ] **UI:** **Give this RTL-SDR a unique serial…** with a confirmation that names the dongle, the old serial, the new one and the
  backup path, plus a warning that the dongle must be unplugged and replugged afterwards. After success, it says to replug. Add a
  **Restore from backup…** action.
- [ ] Commit: `RTL-SDR: give a dongle a unique serial, with a backup`

### Task 6: Docs

**Files:** `docs/radios.md` (new), `README.md` (one line), `packaging/proxmox/README.md` (radios added later show up, plus the notice).

- [ ] Write about telling radios apart, names, finding a radio, the new-radio notice, and unique RTL serials with backup and restore.
  Run the fast suite. Commit: `docs: telling radios apart`
