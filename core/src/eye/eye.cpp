// The Eye's live model (core/include/dect2/eye/eye.h).
#include "dect2/eye/eye.h"
#include "dect2/automation/engine.h"
#include "dect2/msgdb/msgdb.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace dect2::eye {

namespace au = automation;

const char* groupName(int g) {
    static const char* const n[kGroups] = {"Aircraft", "Air data", "Ships", "APRS", "Sondes", "Mesh / LoRa", "Pagers", "Bluetooth", "Drones"};
    return g >= 0 && g < kGroups ? n[g] : "";
}

int groupOfType(const std::string& type) {
    switch (au::familyOf(type)) {
    case au::FAdsb: return GAircraft;
    case au::FAcars: case au::FVdl2: case au::FHfdl: return GAirData;
    case au::FAis: case au::FDsc: return GShips;
    case au::FAprs: return GAprs;
    case au::FSonde: return GSondes;
    case au::FLora: return GMesh;
    case au::FPager: return GPagers;
    case au::FBle: return GBluetooth;
    case au::FRid: return GDrones;
    default: return -1;
    }
}

uint32_t familyMask() {
    uint32_t m = 0;
    for (int f : {au::FAdsb, au::FAcars, au::FVdl2, au::FHfdl, au::FAis, au::FDsc, au::FAprs, au::FSonde, au::FLora, au::FPager, au::FBle, au::FRid})
        m |= 1u << f;
    return m;
}

namespace {

std::string field(const au::Event& ev, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        auto it = ev.f.find(k);
        if (it == ev.f.end() || it->second.null()) continue;
        std::string v = it->second.text();
        while (!v.empty() && v.back() == ' ') v.pop_back();
        if (!v.empty()) return v;
    }
    return {};
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool inMask(uint32_t mask, int g) { return mask == 0 || (mask >> g & 1); }

}  // namespace

void Overview::ingest(const au::Event& ev) {
    const int g = groupOfType(ev.type);
    if (g < 0) return;
    msgdb::Record r;
    if (!msgdb::fromEvent(ev, r)) return;   // voice, encrypted, or not a message type
    if (r.ident.empty()) r.ident = field(ev, {"callsign", "flight"});
    if (r.ident.empty()) r.ident = r.mode.empty() ? ev.type : r.mode;   // a contact with no identity of its own (an anonymous packet)
    const std::string label = field(ev, {"callsign", "flight", "name"});
    std::string text = r.text == label ? std::string() : r.text;
    for (char& c : text) if (c == '\n' || c == '\r' || c == '\t') c = ' ';

    std::lock_guard<std::mutex> lk(mu_);
    total_++;
    version_++;
    const int64_t sec = (int64_t)std::floor(r.time);
    const size_t slot = (size_t)(((sec % kSlots) + kSlots) % kSlots);
    if (slotSec_[slot] != sec) { slotSec_[slot] = sec; slotCount_[slot].fill(0); }
    slotCount_[slot][(size_t)g]++;

    const std::string key = std::to_string(g) + "|" + r.ident;
    auto it = contacts_.find(key);
    const bool first = it == contacts_.end();
    if (first) {
        if (contacts_.size() >= lim_.maxContacts) {   // forget the contact heard least recently
            auto old = std::min_element(contacts_.begin(), contacts_.end(), [](const auto& a, const auto& b) { return a.second.lastSeen < b.second.lastSeen; });
            contacts_.erase(old);
        }
        Contact c;
        c.group = g; c.mode = r.mode; c.ident = r.ident; c.firstSeen = r.time;
        it = contacts_.emplace(key, std::move(c)).first;
    }
    Contact& c = it->second;
    const bool newText = !text.empty() && text != c.text;
    c.lastSeen = std::max(c.lastSeen, r.time);
    c.events++;
    if (!label.empty()) c.label = label;
    if (!r.mode.empty()) c.mode = r.mode;
    if (newText) c.text = text;
    if (r.hasPos) { c.hasPos = true; c.lat = r.lat; c.lon = r.lon; c.posTime = r.time; }

    if (first || newText) {
        Row row;
        row.time = r.time; row.group = g; row.mode = c.mode; row.ident = c.ident; row.label = c.label;
        row.text = newText ? text : std::string("first seen");
        row.freqHz = r.freqHz; row.first = first;
        row.hasPos = r.hasPos; row.lat = r.lat; row.lon = r.lon;
        row.seq = ++seq_;
        rows_.push_back(std::move(row));
        while (rows_.size() > lim_.maxRows) rows_.pop_front();
    }
}

std::vector<Row> Overview::timeline(size_t max, uint32_t groupMask, const std::string& filter) const {
    const std::string q = lower(filter);
    std::vector<Row> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = rows_.rbegin(); it != rows_.rend() && out.size() < max; ++it) {
        if (!inMask(groupMask, it->group)) continue;
        if (!q.empty() && lower(it->mode + "\n" + it->ident + "\n" + it->label + "\n" + it->text).find(q) == std::string::npos) continue;
        out.push_back(*it);
    }
    return out;
}

std::vector<Contact> Overview::contacts(double now, double maxAgeSec, uint32_t groupMask, const std::string& filter) const {
    const std::string q = lower(filter);
    std::vector<Contact> out;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& kv : contacts_) {
            const Contact& c = kv.second;
            if (c.lastSeen < now - maxAgeSec || !inMask(groupMask, c.group)) continue;
            if (!q.empty() && lower(c.mode + "\n" + c.ident + "\n" + c.label + "\n" + c.text).find(q) == std::string::npos) continue;
            out.push_back(c);
        }
    }
    std::sort(out.begin(), out.end(), [](const Contact& a, const Contact& b) { return a.lastSeen > b.lastSeen; });
    return out;
}

std::array<GroupStat, kGroups> Overview::stats(double now, double windowSec) const {
    std::array<GroupStat, kGroups> s{};
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : contacts_)
        if (kv.second.lastSeen >= now - windowSec) s[(size_t)kv.second.group].active++;
    const int64_t to = (int64_t)std::floor(now), from = to - (int64_t)std::min<double>(windowSec, kSlots - 1);
    for (size_t i = 0; i < (size_t)kSlots; i++)
        if (slotSec_[i] > from && slotSec_[i] <= to)
            for (int g = 0; g < kGroups; g++) s[(size_t)g].events += slotCount_[i][(size_t)g];
    return s;
}

uint64_t Overview::total() const {
    std::lock_guard<std::mutex> lk(mu_);
    return total_;
}

uint64_t Overview::version() const {
    std::lock_guard<std::mutex> lk(mu_);
    return version_;
}

void Overview::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    rows_.clear();
    contacts_.clear();
    std::fill(slotSec_.begin(), slotSec_.end(), -1);
    total_ = 0;
    version_++;
}

Overview& global() {
    static Overview o;
    return o;
}

}  // namespace dect2::eye
