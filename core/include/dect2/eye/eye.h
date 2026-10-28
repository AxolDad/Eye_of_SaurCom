// The Eye: one live picture of everything the station is decoding, across modes. Every decoder event (the automation events, the same ones the
// message database keeps) becomes a contact (aircraft, ship, APRS station, sonde, pager address, mesh node ...) with its last position, and the
// things worth reading become rows of a timeline: a contact seen for the first time, and a message whose text changed. Position updates only move
// the contact, so an aircraft reporting every 2 s does not flood the timeline. Thread safe: ingest() runs on the decoders' threads.
#pragma once
#include "dect2/automation/event.h"
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::eye {

// What a contact is, for the chips, the colours and the filters.
enum Group { GAircraft, GAirData, GShips, GAprs, GSondes, GMesh, GPagers, GBluetooth, GDrones, GFixes, kGroups };
const char* groupName(int g);             // "Aircraft", "Air data" ...
int groupOfType(const std::string& type);  // -1 for a type the Eye does not show
uint32_t familyMask();                     // the automation families the Eye listens to (bit = 1 << automation::Family)

struct Row {
    double time = 0;
    int group = 0;
    std::string site;     // the station network site the event came from ("" = this station)
    std::string mode;     // "ADS-B"
    std::string ident;    // "4CA2D1"
    std::string label;    // a friendlier name when the event has one: call sign, ship name ("RYR4LK")
    std::string text;     // what happened, one line ("first seen", or the message)
    double freqHz = 0;
    bool first = false;   // the contact's first sighting
    bool hasPos = false;
    double lat = 0, lon = 0;
    uint64_t seq = 0;     // the row's number, unique and stable while newer rows arrive (an ID for the list)
};

struct Contact {
    int group = 0;
    std::string mode, ident, label, text;   // text: the last message or summary
    std::vector<std::string> sites;         // every site that reported the contact, sorted ("" = this station)
    double firstSeen = 0, lastSeen = 0;
    int64_t events = 0;
    bool hasPos = false;
    double lat = 0, lon = 0;
    double posTime = 0;
};

struct GroupStat {
    int active = 0;       // contacts heard within the window
    int events = 0;       // events within the window
};

struct Limits {
    size_t maxRows = 2000;       // the timeline keeps the newest rows
    size_t maxContacts = 5000;   // the least recently heard go first
};

class Overview {
public:
    explicit Overview(Limits l = Limits()) : lim_(l) {}
    // One decoder event. Ignores types outside the Eye's groups, and voice or encrypted content (like the message database).
    void ingest(const automation::Event& ev);

    // A signal located from bearings of several sites (the caller does the fixing, so the Eye stays free of the DF code): makes or moves the
    // "Located" contact of that frequency; a row only the first time or when its text changes.
    void ingestLocated(double time, double freqHz, double lat, double lon, double e95M, const std::vector<std::string>& sites);

    // Newest first. groupMask: bit = 1 << Group (0 = all). site: only that site's rows or contacts (empty = all sites; a local event has site ""). filter: a case-insensitive substring of mode, ident, label or text (empty = all).
    std::vector<Row> timeline(size_t max, uint32_t groupMask = 0, const std::string& filter = std::string(),
                         const std::string& site = std::string()) const;
    // Contacts heard at or after now - maxAgeSec, most recently heard first; filter as for timeline().
    std::vector<Contact> contacts(double now, double maxAgeSec, uint32_t groupMask = 0, const std::string& filter = std::string(),
                                  const std::string& site = std::string()) const;
    std::array<GroupStat, kGroups> stats(double now, double windowSec) const;   // windowSec up to an hour
    uint64_t total() const;     // events ingested since the start (or the last clear)
    uint64_t version() const;   // changes whenever ingest() or clear() changed something: redraw only then
    void clear();

private:
    static constexpr int kSlots = 3600;           // one per second: the per-group event counts of the last hour
    mutable std::mutex mu_;
    Limits lim_;
    std::deque<Row> rows_;                        // oldest first
    std::map<std::string, Contact> contacts_;     // key: group|ident
    std::vector<int64_t> slotSec_ = std::vector<int64_t>(kSlots, -1);   // the second each slot counts
    std::vector<std::array<int, kGroups>> slotCount_ = std::vector<std::array<int, kGroups>>(kSlots);
    uint64_t total_ = 0;
    uint64_t version_ = 0;
    uint64_t seq_ = 0;
};

// The program-wide Eye the app shows (fed from the automation engine's overview hook).
Overview& global();

}  // namespace dect2::eye
