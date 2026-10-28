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

struct RadioDiff { std::vector<DeviceInfo> added, removed; };
// Radios are matched by their key as identifyRadios() gives it (so identical-serial radios on different ports differ).
RadioDiff diffRadios(const std::vector<DeviceInfo>& before, const std::vector<DeviceInfo>& after);

}  // namespace dect2
