// The user's names for their radios and the identity of each entry in a receiver's radio list (model, serial, USB port, #n for look-alikes).
// One app-wide RadioLabels, read from <dataDir>/radios/names.json at start and written only by the UI thread (setRadioName and the
// label migration inside radioRows), so there is a single saver. Display names are text: show them with %s / TextUnformatted only.
#pragma once
#include "dect2/source/radio_id.h"
#include <string>
#include <vector>

struct App;

namespace radionames {

void load();                                  // once, at start (a missing or damaged file gives no names and a note in loadError())
const std::string& loadError();
std::string path();
unsigned generation();                        // changes when a name is set, so cached rows are rebuilt

std::string labelFor(const std::string& key);   // "" when none

// The user's name for one radio (cleaned of control characters and cut to 64 bytes; empty removes it). Writes the file. False and err on a write error.
bool setName(const std::string& key, const std::string& name, std::string& err);

// After a dongle got a new serial: its name (under row.key) moves to the key of the new serial, and the file is saved. False and err on a write error.
bool carryLabel(const dect2::RadioRow& row, const std::string& newSerial, std::string& err);
// After a backup put an old serial back: the name moves back to the key that serial gives among the attached radios (rtlCarryLabelBack).
bool carryLabelBack(const dect2::RadioRow& row, const std::vector<dect2::DeviceInfo>& attached, const std::string& restoredSerial, std::string& err);

// One row per entry of devices (same order). Only radios get a key; other sources have an empty key and are shown by their own name.
// Looks at the labels, moves a label from the "sn:" to the "usb:" key when a serial has become shared, and saves when it did.
std::vector<dect2::RadioRow> rowsFor(const std::vector<dect2::DeviceInfo>& devices);

}  // namespace radionames

// The row of a.devices[idx] (rebuilt when the list or a name changed); nullptr when idx is out of range.
const dect2::RadioRow* radioRow(const App& a, int idx);
// What to show the user for a.devices[idx]: the user's name, else "<name> · SN <serial> · USB <port>" (+ " #n"); a non-radio source keeps its name.
std::string radioShownName(const App& a, int idx);
// The same for a device of the list a.devices (by pointer into it); falls back to its own name.
std::string radioShownName(const App& a, const dect2::DeviceInfo& d);
// The key to save with a receiver or workspace ("" for a source that is not a radio).
std::string radioKeyAt(const App& a, int idx);
// The entry of a.devices for a saved radio (dect2::findRadioRow): by key first, then by kind, serial and USB port of `was` (the entry as it was,
// when known), then by the internal name when only one entry has it. -1 when none, dect2::kRadioAmbiguous when several look-alikes share
// the name (never one of them by surprise); the first two entries (synthetic, file) are skipped.
int findRadio(const App& a, const std::string& key, const std::string& name, const dect2::DeviceInfo* was = nullptr);
// Logs that the receiver starts a.devices[idx], with the user's name when it differs from the internal one: starting "Attic RTL" (RTL-SDR ...).
void logRadioStart(App& a, int idx);
// True when two or more radios here share a serial; fills a warning for the radio settings / list.
std::string sharedSerialWarning(const App& a);
