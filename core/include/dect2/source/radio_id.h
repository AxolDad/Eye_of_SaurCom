// Telling radios apart: a stable key per radio (serial, or USB port when serials are shared), duplicate numbering, user-given names
// kept in a small JSON file, and the difference between two radio lists.
#pragma once
#include "dect2/source/source.h"
#include <map>
#include <string>
#include <vector>

namespace dect2 {

// "<kind>:<board>:sn:<serial>"; with an empty or shared (factory default / repeated) serial "<kind>:<board>:usb:<usbPath>";
// with no path either "<kind>:<board>:name:<name>". kind is the DeviceInfo::Kind number.
// This overload only knows the one radio, so it treats just the factory-default serials as shared; identifyRadios() also sees repeats.
std::string radioKey(const DeviceInfo& d);

// User-given names by radio key, in a JSON file written privately (mode 0600 on POSIX).
class RadioLabels {
public:
    static constexpr size_t kMaxLen = 64;
    // False with err when the file is missing, unreadable or not what we wrote; the labels are empty then.
    bool load(const std::string& path, std::string& err);
    // Writes a temporary file then renames it over path.
    bool save(const std::string& path, std::string& err) const;
    std::string get(const std::string& key) const;
    // Cut to 64 bytes (never inside a UTF-8 character); an empty name removes the label.
    void set(const std::string& key, const std::string& name);
    void remove(const std::string& key);
private:
    std::map<std::string, std::string> m_;
};

struct RadioRow {
    DeviceInfo dev;
    std::string key;
    std::string label;      // the user's name, empty when none
    int dupIndex = 0;       // 0 = unique, else 1.. among radios that look the same (ordered by USB path)
    bool sharedSerial = false;
};

// One row per radio, in input order. Shared serials: seen on two or more radios of the same kind and board, or a known
// factory default (00000001, 0, empty) even when only one radio is attached.
std::vector<RadioRow> identifyRadios(const std::vector<DeviceInfo>& devs, const RadioLabels& labels);

// The label if set, else "<name> · SN <serial> · USB <path>" without empty parts, plus " #n" for a duplicate.
// A label is shown as it is: the user chose it to tell the radios apart.
std::string radioDisplayName(const RadioRow& r);

// A radio's key flips from "sn:<serial>" to "usb:<path>" when a second radio with that serial appears. For rows that have a usb: key and no
// label, while a label exists under the "sn:" key of the same kind, board and serial, that label is copied to the usb: key of one radio (the
// lowest dupIndex, then the lowest USB path), saved in labels (the caller saves the file) and set on the row. The sn: label is removed (moved, not copied), so this happens at most once:
// a name cleared afterwards stays cleared, and a single radio with that serial later starts with no name. A group where any radio already has
// a label is left alone.
// Returns true when it changed anything.
bool migrateLabels(std::vector<RadioRow>& rows, RadioLabels& labels);

// A name typed by the user, made safe to store and show: control characters (newlines, tabs, DEL, C1 controls) are dropped and the ends
// are trimmed. Other text, UTF-8 included, is kept as it is; it must still only ever be shown as text, never as a format string.
std::string cleanRadioName(const std::string& s);

// identifyRadios over a whole source list (test signal, file, sound cards, radios): one row per entry in the same order. Only entries that
// are radios get a key, a number and a label; the rest keep their own name. Runs migrateLabels (labels change; *migrated says whether).
std::vector<RadioRow> identifySources(const std::vector<DeviceInfo>& devs, RadioLabels& labels, bool* migrated = nullptr);

// Where a radio a receiver was bound to is in a new list, so it keeps that exact radio: the row with its key; else (its key can flip between
// "sn:" and "usb:" when a look-alike comes or goes) the row with its kind, board, serial and USB port (only when `was` has a port); else the row
// with its internal name (and was's kind and serial, when given), but only when exactly one row has that name. Rows before `first` are skipped. -1 when none; kRadioAmbiguous when the
// name matches several rows (a look-alike is never picked by its name: the caller keeps or flags the choice). was may be nullptr.
constexpr int kRadioAmbiguous = -2;
int findRadioRow(const std::vector<RadioRow>& rows, const std::string& key, const DeviceInfo* was, const std::string& name, size_t first = 0);

struct RadioDiff { std::vector<DeviceInfo> added, removed; };
// Radios are matched by their key as identifyRadios() gives it (so identical-serial radios on different ports differ).
RadioDiff diffRadios(const std::vector<DeviceInfo>& before, const std::vector<DeviceInfo>& after);

}  // namespace dect2
