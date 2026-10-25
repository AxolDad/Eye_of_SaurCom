// The flight history on disk: every flight this receiver heard, the track of each one, a dossier per aircraft that the user can add to,
// and the queries that answer what is actually worth knowing about one's own airspace.
//
// A separate SQLite file from the message store (msgdb): the shapes have nothing in common, and a user who wants to keep years of flights
// while pruning messages weekly can. Tables:
//   flights   one row per flight (flights_proto.h Flight), with the local hour and weekday it fell in, so the time questions are indexed
//   points    the track of each flight
//   aircraft  one row per address: what is known about it, what it adds up to, and the user's own notes, tags, pictures and corrections
//   facts     anything else the user researched about an aircraft, as free key and value pairs
//   events    notable moments: an emergency squawk, a first sighting, a flight off its usual path, or one the user marked
//
// Everything the program inferred is kept apart from everything the user entered, and a user's correction always wins when the two
// disagree. Nothing here reaches the network: the dossier is the user's own file.
#pragma once
#include "flights_proto.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2::flights {

// ---------------------------------------------------------------- what the store keeps

struct Limits {
    int64_t maxFlights = 2000000;     // 0 = no limit
    double maxAgeDays = 0;            // 0 = keep forever
    int64_t maxBytes = 2048ll << 20;  // size cap of the file, 0 = none; the oldest flights go first
    bool keepTracks = true;           // false: keep the flights' figures but drop their points (much smaller)
    double trackMaxAgeDays = 0;       // 0 = keep tracks as long as their flights; else drop points older than this
};

struct Stats {
    int64_t flights = 0, points = 0, aircraft = 0, events = 0;
    int64_t bytes = 0;
    double oldest = 0, newest = 0;
};

// What is known about one aircraft: what the program worked out, and what the user said.
struct AircraftRecord {
    uint32_t icao = 0;
    // identity, from AdsbRegDb and the ADS-B emitter category
    std::string registration, type, operatorName, category, country;
    bool rotorcraft = false;
    // what it adds up to
    double firstSeen = 0, lastSeen = 0;
    int64_t flightCount = 0;
    double airborneSec = 0;           // time heard airborne, which is not the airframe's total
    double distanceNm = 0;
    int64_t messages = 0;
    int maxAltFt = 0;
    float maxSpeedKt = 0;
    float meanCruiseAltFt = 0, meanCruiseSpeedKt = 0;
    std::string homeIcao;             // the airport it begins and ends at most often
    int64_t homeCount = 0;
    double meanGroundSec = 0;         // between consecutive flights that both touched the same airport
    // the kind of operation, inferred and as the user corrected it
    OperationKind kind = OpUnknown;
    float kindConfidence = 0;
    std::string kindBasis;
    int kindUser = -1;                // the user's own answer (OperationKind), -1 when they have not said
    OperationKind effectiveKind() const { return kindUser >= 0 ? (OperationKind)kindUser : kind; }
    // the user's dossier
    std::string notes;
    std::vector<std::string> tags;
    std::vector<std::string> pictures;   // paths to local files the user attached
};

// A free fact the user researched and recorded ("built", "1998"), kept apart from anything the program decided.
struct Fact { std::string key, value; double edited = 0; };

// A moment worth keeping. `user` is true when the user wrote it rather than the program.
struct Event {
    int64_t id = 0;
    uint32_t icao = 0;
    int64_t flightId = 0;
    double t = 0;
    std::string kind;    // "emergency", "squawk", "first", "off-route", "note", "special"
    std::string text;
    bool user = false;
};

// ---------------------------------------------------------------- asking questions

struct Filter {
    double since = 0, until = 0;      // Unix seconds, 0 = open
    uint32_t icao = 0;                // one aircraft, 0 = any
    std::string registration;         // substring, "" = any
    std::string callsign;             // prefix, "" = any
    std::string airport;              // flights that began or ended here, "" = any
    std::string op3;                  // the three letter operator code of the call sign
    int kind = -1;                    // OperationKind, -1 = any
    int minHour = -1, maxHour = -1;   // local hour of the day
    int dow = -1;                     // local day of week, 0 Sunday
    bool emergenciesOnly = false;
    bool withPositionOnly = false;
    int limit = 500, offset = 0;
    bool oldestFirst = false;
};

// One line of a ranking, used by every "most active" question so the screens stay simple.
struct Ranked {
    std::string key;       // the airport, operator code, tag, route or whatever was grouped by
    std::string label;     // something readable, where the store can work one out
    uint32_t icao = 0;     // when the ranking is of aircraft
    int64_t count = 0;     // flights
    double seconds = 0;    // time airborne, where it means something
    double value = 0;      // whatever the question asked for (mean altitude, deviation, ...)
};

// A bucket of a histogram (hour of day, weekday, altitude band, speed band).
struct Bucket { int bin = 0; int64_t count = 0; double value = 0; };

// How regular an aircraft's flying is: how many different paths it has flown and how much of its flying is on the commonest one.
struct Regularity {
    int64_t flights = 0;
    int64_t distinctRoutes = 0;
    std::string commonest;         // the route signature
    int64_t commonestCount = 0;
    float share = 0;               // commonestCount / flights
};

// A flight that stands out, and why. The score is only a ranking aid, not a measurement of anything.
struct Anomaly {
    int64_t flightId = 0;
    uint32_t icao = 0;
    double t = 0;
    std::string registration, callsign;
    float score = 0;
    std::string reason;
};

class Db {
public:
    Db();
    ~Db();
    bool open(const std::string& path, std::string* err = nullptr);   // creates the file and the tables; ":memory:" for tests
    void close();
    bool isOpen() const;
    std::string path() const;

    // ---- writing
    // Adds a finished flight and its track, updates the aircraft's dossier and raises any events it earns. Fills f.id. The facts are what
    // the caller knows about the aircraft (from AdsbRegDb); anything already in the dossier that the user edited is left alone.
    bool addFlight(Flight& f, const std::vector<Point>& pts, const AircraftFacts& facts);
    // The user's own parts of a dossier. Each only touches what it is given.
    bool setNotes(uint32_t icao, const std::string& notes);
    bool setTags(uint32_t icao, const std::vector<std::string>& tags);
    bool setPictures(uint32_t icao, const std::vector<std::string>& paths);
    bool setKindOverride(uint32_t icao, int kind);        // -1 to take the program's answer again
    bool setFact(uint32_t icao, const std::string& key, const std::string& value);   // "" value removes it
    bool addEvent(const Event& e);
    bool removeEvent(int64_t id);

    void setLimits(const Limits& l);
    Limits limits() const;
    int64_t prune();
    void clear();
    Stats stats() const;

    // ---- flights
    std::vector<Flight> flights(const Filter& f) const;
    int64_t countFlights(const Filter& f) const;
    bool flight(int64_t id, Flight& out) const;
    std::vector<Point> track(int64_t flightId) const;    // for drawing and replay

    // ---- aircraft and their dossiers
    bool aircraft(uint32_t icao, AircraftRecord& out) const;
    std::vector<AircraftRecord> aircraftList(const Filter& f) const;
    std::vector<Fact> facts(uint32_t icao) const;
    std::vector<Event> events(const Filter& f) const;

    // ---- "what is in my airspace the most", and the other rankings
    // Aircraft by how often they were heard, with their time airborne. The seconds are heard-airborne, not airframe hours.
    std::vector<Ranked> topAircraft(const Filter& f, int limit = 25) const;
    std::vector<Ranked> topAirports(const Filter& f, int limit = 25) const;     // airports flights began or ended at
    std::vector<Ranked> topOperators(const Filter& f, int limit = 25) const;    // the three letter code of airline call signs
    std::vector<Ranked> topTypes(const Filter& f, int limit = 25) const;        // aircraft types
    std::vector<Ranked> topRoutes(const Filter& f, int limit = 25) const;       // origin -> destination pairs
    std::vector<Ranked> byKind(const Filter& f) const;                          // flights per operation kind
    // Where the flights of one kind went or came from (medical flights to which hospitals' airports, military from where).
    std::vector<Ranked> kindAirports(const Filter& f, int kind, bool destinations, int limit = 25) const;

    // ---- distributions
    std::vector<Bucket> hourHistogram(const Filter& f) const;                   // local hour of the day, 0 - 23
    std::vector<Bucket> weekdayHistogram(const Filter& f) const;                // 0 Sunday - 6 Saturday
    std::vector<Bucket> altitudeHistogram(const Filter& f, int bandFt = 2000) const;
    std::vector<Bucket> speedHistogram(const Filter& f, int bandKt = 25) const;
    std::vector<Bucket> dailyCounts(const Filter& f) const;                     // bin = days since the Unix epoch

    // ---- patterns
    Regularity regularity(uint32_t icao) const;
    // Flights that stand out: off the aircraft's usual path, at an unusual hour for it, an emergency, or an unusually long or high flight.
    std::vector<Anomaly> anomalies(const Filter& f, int limit = 50) const;
    // How the rate of something now compares with the weeks before it: flights in the window against the mean of the same length of
    // window over `backWeeks` weeks. value is the ratio (1 = as usual), count the flights in the window.
    Ranked rateVsUsual(const Filter& f, double windowSec = 86400, int backWeeks = 6) const;
    // The gaps an aircraft spent on the ground between flights that both touched the same airport, in seconds.
    std::vector<double> groundGaps(uint32_t icao) const;

    // ---- export, for the user's own analysis elsewhere
    std::string exportFlights(const Filter& f, const std::string& format) const;   // "csv" or "json"
    std::string exportTrack(int64_t flightId, const std::string& format) const;    // "csv", "json" or "gpx"

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// The program-wide store, opened by the application at start.
Db& global();

} // namespace dect2::flights
