#include "dect2/source/radio_id.h"
#include "dect2/automation/json_mini.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>

namespace dect2 {

namespace {

namespace aj = automation::json;

// serials that many dongles ship with, so they say nothing about which radio this is
bool isFactorySerial(const std::string& s) { return s.empty() || s == "00000001" || s == "0"; }

std::string keyFor(const DeviceInfo& d, bool shared) {
    std::string k = std::to_string(static_cast<int>(d.kind)) + ":" + d.board + ":";
    if (!shared) return k + "sn:" + d.serial;
    if (!d.usbPath.empty()) return k + "usb:" + d.usbPath;
    return k + "name:" + d.name;
}

// radios that look the same to the user: same kind, board and serial (an empty serial also needs the same name)
using GroupKey = std::tuple<int, std::string, std::string, std::string>;
GroupKey groupOf(const DeviceInfo& d) {
    return {static_cast<int>(d.kind), d.board, d.serial, d.serial.empty() ? d.name : std::string()};
}

}  // namespace

std::string radioKey(const DeviceInfo& d) { return keyFor(d, isFactorySerial(d.serial)); }

// ---- labels ----

bool RadioLabels::load(const std::string& path, std::string& err) {
    err.clear();
    m_.clear();
    std::ifstream f(u8file(path), std::ios::binary);
    if (!f) { err = "cannot read " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    aj::Value root;
    std::string perr;
    if (!aj::parse(ss.str(), root, perr)) { err = "radio names file is damaged (" + perr + ")"; return false; }
    const aj::Value* labels = root.type == aj::Value::Obj ? root.get("labels") : nullptr;
    if (!labels || labels->type != aj::Value::Obj) { err = "radio names file is damaged (no labels)"; return false; }
    for (const auto& [k, v] : labels->o)
        if (v.type == aj::Value::Str) set(k, cleanRadioName(v.s));   // anything else is skipped; a hand-edited name loses its control characters
    return true;
}

bool RadioLabels::save(const std::string& path, std::string& err) const {
    namespace fs = std::filesystem;
    err.clear();
    aj::Value root = aj::Value::object();
    aj::Value labels = aj::Value::object();
    for (const auto& [k, v] : m_) labels.set(k, aj::Value::string(v));
    root.set("version", aj::Value::number(1));
    root.set("labels", std::move(labels));
    std::error_code ec;
    const fs::path p = u8file(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    // a new file that is private from the start, renamed over the old one, so a crash never leaves half a file
    const fs::path tmp = fs::path(p).concat(".tmp");
    fs::remove(tmp, ec);
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { err = "cannot write " + path; return false; }
        o.close();
#ifndef _WIN32
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
#endif
        std::ofstream w(tmp, std::ios::binary | std::ios::trunc);
        w << aj::dump(root, true) << "\n";
        w.flush();
        if (!w) { err = "cannot write " + path; fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { err = "cannot replace " + path + ": " + ec.message(); fs::remove(tmp, ec); return false; }
#ifndef _WIN32
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
#endif
    return true;
}

std::string RadioLabels::get(const std::string& key) const {
    auto it = m_.find(key);
    return it == m_.end() ? std::string() : it->second;
}

void RadioLabels::set(const std::string& key, const std::string& name) {
    std::string n = name;
    if (n.size() > kMaxLen) {
        size_t cut = kMaxLen;
        while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) --cut;   // do not end inside a UTF-8 character
        n.resize(cut);
    }
    if (n.empty()) m_.erase(key);
    else m_[key] = std::move(n);
}

void RadioLabels::remove(const std::string& key) { m_.erase(key); }

// ---- identity ----

std::vector<RadioRow> identifyRadios(const std::vector<DeviceInfo>& devs, const RadioLabels& labels) {
    // how many radios of each kind and board carry each serial
    std::map<std::tuple<int, std::string, std::string>, int> serialCount;
    for (const DeviceInfo& d : devs) ++serialCount[{static_cast<int>(d.kind), d.board, d.serial}];
    std::map<GroupKey, std::vector<size_t>> groups;
    std::vector<RadioRow> rows(devs.size());
    for (size_t i = 0; i < devs.size(); i++) {
        const DeviceInfo& d = devs[i];
        RadioRow& r = rows[i];
        r.dev = d;
        r.sharedSerial = isFactorySerial(d.serial) || serialCount[{static_cast<int>(d.kind), d.board, d.serial}] > 1;
        r.key = keyFor(d, r.sharedSerial);
        r.label = labels.get(r.key);
        groups[groupOf(d)].push_back(i);
    }
    // number the look-alikes by USB port so the numbers do not move when the list order does
    for (auto& [g, idx] : groups) {
        if (idx.size() < 2) continue;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return devs[a].usbPath < devs[b].usbPath; });
        for (size_t n = 0; n < idx.size(); n++) rows[idx[n]].dupIndex = static_cast<int>(n) + 1;
    }
    return rows;
}

std::string radioDisplayName(const RadioRow& r) {
    if (!r.label.empty()) return r.label;
    std::string s = r.dev.name;
    auto add = [&](const std::string& part) {
        if (part.empty()) return;
        if (!s.empty()) s += " \xC2\xB7 ";
        s += part;
    };
    if (!r.dev.serial.empty()) add("SN " + r.dev.serial);
    if (!r.dev.usbPath.empty()) add("USB " + r.dev.usbPath);
    if (r.dupIndex > 0) s += " #" + std::to_string(r.dupIndex);
    return s;
}

std::string cleanRadioName(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) continue;
        if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) >= 0x80 && static_cast<unsigned char>(s[i + 1]) <= 0x9F) { i++; continue; }
        o += s[i];
    }
    size_t b = 0, e = o.size();
    while (b < e && o[b] == ' ') b++;
    while (e > b && o[e - 1] == ' ') e--;
    return o.substr(b, e - b);
}

bool migrateLabels(std::vector<RadioRow>& rows, RadioLabels& labels) {
    // radios of one kind, board and serial, grouped by the "sn:" key they had while alone
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < rows.size(); i++) {
        const DeviceInfo& d = rows[i].dev;
        if (d.serial.empty() || isFactorySerial(d.serial)) continue;   // these never had an sn: key
        if (rows[i].key.find(":usb:") == std::string::npos) continue;
        groups[keyFor(d, false)].push_back(i);
    }
    bool changed = false;
    for (auto& [snKey, idx] : groups) {
        const std::string name = labels.get(snKey);
        if (name.empty()) continue;
        bool anyLabel = false;
        for (size_t i : idx) anyLabel |= !rows[i].label.empty();
        if (anyLabel) continue;
        size_t best = idx[0];
        for (size_t i : idx) {
            const RadioRow &a = rows[i], &b = rows[best];
            if (std::make_pair(a.dupIndex, a.dev.usbPath) < std::make_pair(b.dupIndex, b.dev.usbPath)) best = i;
        }
        labels.set(rows[best].key, name);
        labels.remove(snKey);   // moved, not copied: the migration happens once, so clearing the new name stays cleared
        rows[best].label = labels.get(rows[best].key);
        changed = true;
    }
    return changed;
}

int findRadioRow(const std::vector<RadioRow>& rows, const std::string& key, const DeviceInfo* was, const std::string& name, size_t first) {
    if (!key.empty())
        for (size_t i = first; i < rows.size(); i++) if (rows[i].key == key) return static_cast<int>(i);
    if (was && !was->usbPath.empty())
        for (size_t i = first; i < rows.size(); i++) {
            const DeviceInfo& d = rows[i].dev;
            if (!rows[i].key.empty() && d.kind == was->kind && d.board == was->board && d.serial == was->serial && d.usbPath == was->usbPath) return static_cast<int>(i);
        }
    if (name.empty()) return -1;
    int found = -1;
    for (size_t i = first; i < rows.size(); i++) {
        if (rows[i].dev.name != name) continue;
        if (was && (rows[i].dev.kind != was->kind || rows[i].dev.serial != was->serial)) continue;   // a name says less than the serial it had
        if (found >= 0) return kRadioAmbiguous;
        found = static_cast<int>(i);
    }
    return found;
}

std::vector<RadioRow> identifySources(const std::vector<DeviceInfo>& devs, RadioLabels& labels, bool* migrated) {
    std::vector<DeviceInfo> radios;
    std::vector<size_t> at;
    for (size_t i = 0; i < devs.size(); i++)
        if (devs[i].isRadio()) { radios.push_back(devs[i]); at.push_back(i); }
    std::vector<RadioRow> rr = identifyRadios(radios, labels);
    const bool moved = migrateLabels(rr, labels);
    if (migrated) *migrated = moved;
    std::vector<RadioRow> out(devs.size());
    for (size_t i = 0; i < devs.size(); i++) out[i].dev = devs[i];
    for (size_t k = 0; k < at.size(); k++) out[at[k]] = std::move(rr[k]);
    return out;
}

RadioDiff diffRadios(const std::vector<DeviceInfo>& before, const std::vector<DeviceInfo>& after) {
    const RadioLabels none;
    const auto b = identifyRadios(before, none), a = identifyRadios(after, none);
    std::map<std::string, int> inBefore, inAfter;
    for (const auto& r : b) ++inBefore[r.key];
    for (const auto& r : a) ++inAfter[r.key];
    RadioDiff d;
    for (const auto& r : a) if (!inBefore.count(r.key)) d.added.push_back(r.dev);
    for (const auto& r : b) if (!inAfter.count(r.key)) d.removed.push_back(r.dev);
    return d;
}

// ---- short names (tab titles, the device scan line)
namespace {
std::string trimSpaces(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '-' || s.back() == ':')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && s[b] == ' ') b++;
    return s.substr(b);
}
bool endsWith(const std::string& s, const std::string& t) { return t.size() <= s.size() && s.compare(s.size() - t.size(), t.size(), t) == 0; }
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
// the serial, or a tail of it of 4 or more characters, written at the end of the name
bool cutSerialTail(std::string& s, const std::string& serial) {
    if (serial.empty()) return false;
    const std::string ls = lower(s), lser = lower(serial);
    for (size_t k = 0; k + 4 <= lser.size(); k++) {
        const std::string tail = lser.substr(k);
        if (endsWith(ls, " " + tail) || endsWith(ls, ":" + tail)) { s = trimSpaces(s.substr(0, s.size() - tail.size())); return true; }
        if (endsWith(ls, "(" + tail + ")")) { s = trimSpaces(s.substr(0, s.size() - tail.size() - 2)); return true; }
    }
    return false;
}
std::string serialTail(const std::string& serial) { return serial.size() > 8 ? serial.substr(serial.size() - 8) : serial; }
}  // namespace

std::string radioModelName(const DeviceInfo& d) {
    std::string s = trimSpaces(d.name);
    for (int guard = 0; guard < 4; guard++) {   // driver notes and the serial, from the end, in any order
        bool cut = false;
        if (!s.empty() && s.back() == ')') {
            const size_t open = s.rfind('(');
            if (open != std::string::npos && s.find("experimental", open) != std::string::npos) { s = trimSpaces(s.substr(0, open)); cut = true; }
        }
        if (!cut) { const size_t sep = s.find(" :: "); if (sep != std::string::npos && sep > 0) { s = trimSpaces(s.substr(0, sep)); cut = true; } }   // SoapySDR's "label :: serial"
        if (!cut) cut = cutSerialTail(s, d.serial);
        if (!cut) break;
    }
    const std::string l = lower(s);
    if (l.rfind("rtl283", 0) == 0 || l.rfind("generic rtl", 0) == 0 || l == "rtl-sdr" || l == "rtlsdr") s = "RTL-SDR";
    return s.empty() ? d.name : s;
}

std::vector<std::string> radioShortNames(const std::vector<RadioRow>& rows, bool withSerial) {
    std::vector<std::string> v;
    v.reserve(rows.size());
    for (const auto& r : rows) {
        if (r.key.empty() && !r.dev.isRadio()) { v.push_back(r.dev.name); continue; }
        if (!r.label.empty()) { v.push_back(r.label); continue; }
        std::string s = radioModelName(r.dev);
        if (withSerial && !r.dev.serial.empty()) s += " (SN " + serialTail(r.dev.serial) + ")";
        v.push_back(s);
    }
    std::map<std::string, int> count, seen;
    for (size_t i = 0; i < v.size(); i++) if (rows[i].dev.isRadio()) count[v[i]]++;
    for (size_t i = 0; i < v.size(); i++)
        if (rows[i].dev.isRadio() && count[v[i]] > 1) { const std::string base = v[i]; v[i] += " #" + std::to_string(++seen[base]); }
    return v;
}

std::string radioScanSummary(const std::vector<std::string>& radioNames, size_t soundCards, const ScanWords& w) {
    std::string names;
    for (const auto& n : radioNames) { if (!names.empty()) names += ", "; names += n; }
    std::string s;
    // the names go in as arguments: a name with a % in it stays as it is
    auto fmt = [](const char* f, long n, const std::string* str) {
        std::string out;
        for (const char* p = f; *p; p++) {
            if (p[0] == '%' && p[1] == 'd') { out += std::to_string(n); p++; }
            else if (p[0] == '%' && p[1] == 's' && str) { out += *str; p++; }
            else out += *p;
        }
        return out;
    };
    if (radioNames.empty()) s = w.none;
    else if (radioNames.size() == 1) s = fmt(w.one, 1, &names);
    else s = fmt(w.many, (long)radioNames.size(), &names);
    if (soundCards == 1) s += std::string(" \xC2\xB7 ") + w.card;
    else if (soundCards > 1) s += " \xC2\xB7 " + fmt(w.cards, (long)soundCards, nullptr);
    return s;
}

}  // namespace dect2
