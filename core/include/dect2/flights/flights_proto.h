// Flights out of the live aircraft table: where one flight ends and the next begins, what the aircraft was doing at each moment, what the
// flight adds up to, and what kind of operation it looks like. Pure logic, no I/O and no database: the recorder (flights_rec.h) feeds it
// snapshots and the store (flights_db.h) keeps what comes out.
//
// A "flight" here is one continuous period during which this receiver heard an aircraft fly. It is not an airline's idea of a flight: the
// receiver's range decides where it starts and stops, so a flight that only passes overhead begins and ends in the air, and its origin and
// destination are unknown unless it took off or landed within range. Everything this layer reports is therefore marked with how it was
// arrived at, and nothing is presented as certain when it is a guess from the shape of the track.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
struct AdsbAircraft;
struct Airport;
struct Runway;
struct RunwayEnd;
class AirportDb;
}

namespace dect2::flights {

// ---------------------------------------------------------------- points and phases

enum Phase : uint8_t { PhUnknown = 0, PhGround, PhTakeoff, PhClimb, PhCruise, PhDescent, PhApproach, PhLanding };
const char* phaseName(Phase p);          // "ground", "takeoff", "climb", "cruise", "descent", "approach", "landing", ""

// One position report as the recorder saw it. Times are Unix seconds so a track means the same thing after a restart.
struct Point {
    double t = 0;
    double lat = 0, lon = 0;
    int altFt = 0;                       // barometric altitude; 0 with hasAlt false means "not reported", not sea level
    bool hasAlt = false;
    float speedKt = 0; bool hasSpeed = false;
    float trackDeg = 0; bool hasTrack = false;
    int vrateFpm = 0; bool hasVrate = false;
    bool ground = false;                 // the aircraft said it is on the ground
    Phase phase = PhUnknown;             // filled by classifyPhases()
};

// What a flight looks like to the classifier at one moment, so the thresholds live in one place.
struct PhaseLimits {
    int groundAltFt = 150;               // within this of the field elevation (or of the track's lowest point) counts as near the ground
    float groundSpeedKt = 40;            // below this on the ground
    int climbFpm = 300;                  // steady climb or descent beyond this
    int cruiseBandFpm = 300;             // level flight within this
    int takeoffAltFt = 1000;             // the climb off the runway counts as the take-off until this far above the field
    int approachAltFt = 3000;            // a descent below this near an airport is an approach
    float approachRangeNm = 12;          // how near an airport the approach has to be
};

// Fills Point::phase for a whole flight. The vertical rate is used where it is reported and otherwise differentiated from the altitudes,
// smoothed over a few points so one bad altitude does not change the phase. `fieldElevFt` is the elevation to measure "near the ground"
// against (the origin or destination airport when one is known); pass INT_MIN to use the lowest altitude in the track.
void classifyPhases(std::vector<Point>& pts, const PhaseLimits& lim = PhaseLimits(), int fieldElevFt = -1000000);

// Turns the descent into the airport the flight ended at into PhApproach. Separate from classifyPhases() because it needs the airport
// table; run it straight after. A descent over an airport the flight merely passed is left as a descent.
void markApproachPhase(std::vector<Point>& pts, const AirportDb& db, const PhaseLimits& lim = PhaseLimits());

// ---------------------------------------------------------------- what a flight adds up to

// How an origin or destination was arrived at, because a guess from the track must not read like a filed flight plan.
enum class EndKind : uint8_t {
    None = 0,        // the flight began or ended in the air, out of range: nothing can be said
    Airport,         // it was on the ground at a known airport
    NearAirport,     // low and slow next to one, but never reported on the ground
    Overhead,        // still in the air, but the track pointed at an airport it was descending into
};
const char* endKindName(EndKind k);

struct FlightEnd {
    EndKind kind = EndKind::None;
    std::string icao;                    // airport code, "" when none
    std::string runway;                  // the runway end it used, when an approach or departure could be matched
    double t = 0;                        // when it happened
    double lat = 0, lon = 0;
    int altFt = 0;
    float distNm = 0;                    // how far the aircraft was from the airport when this was decided
};

// What an approach measured against a runway looks like. Deviations are signed: lateral positive right of the centreline as seen by the
// pilot, vertical positive above the glide path. Only the part of the track inside the window is measured.
struct ApproachFit {
    bool ok = false;
    std::string airport, runway;
    float glideDeg = 3.0f;               // the path it was compared against
    float fromNm = 10, toNm = 0.5f;      // the window along the centreline
    int points = 0;
    float meanLateralNm = 0, maxLateralNm = 0;
    float meanVerticalFt = 0, maxVerticalFt = 0;
    float thresholdCrossFt = 0;          // height above the threshold where the track crossed it (0 when it did not reach it)
    bool crossed = false;
    float alignedFromNm = 0;             // how far out the track first came within half a mile of the centreline (0: never did)
};

// The whole flight, as it goes into the store. Durations are seconds, altitudes feet, speeds knots, distances nautical miles.
struct Flight {
    int64_t id = 0;                      // the store's row id (0 until written)
    uint32_t icao = 0;                   // the 24-bit address
    std::string callsign;                // the last one heard, trimmed ("" when it never sent one)
    std::string squawk;                  // the last one heard, four octal digits ("" when none)
    double tStart = 0, tEnd = 0;         // first and last message, Unix seconds
    double airborneSec = 0;              // time not on the ground
    size_t points = 0;                   // positions recorded
    uint64_t messages = 0;               // messages heard during the flight
    bool hasAlt = false; int maxAltFt = 0, cruiseAltFt = 0;      // cruise: the altitude it spent longest at while level
    bool hasSpeed = false; float maxSpeedKt = 0, cruiseSpeedKt = 0;
    float distanceNm = 0;                // along the track that was actually heard
    float maxRangeNm = 0, minRangeNm = 0;   // from the receiver (0 when it has no position)
    bool hasRange = false;
    int maxVrateFpm = 0, minVrateFpm = 0;
    FlightEnd origin, destination;
    ApproachFit approach;                // the arrival, when one could be measured
    uint32_t phaseSec[8] = {};           // seconds in each Phase
    bool sawGround = false, sawTakeoff = false, sawLanding = false;
    int emergency = 0;                   // the highest emergency state seen (ADS-B TC 28), 0 none
    bool emergencySquawk = false;        // 7500, 7600 or 7700 at any point
    std::vector<std::string> squawksSeen;
    std::string routeSignature;          // see routeSignature(): equal signatures mean the same path was flown
    uint8_t kind = 0;                    // OperationKind, as inferred at the end of the flight
    float kindConfidence = 0;            // 0 .. 1, an estimate
    std::string kindBasis;               // what the inference rested on, in words, so the user can judge it
    bool complete = false;               // the flight ended (not still being heard)
};

// Sums the points into the flight's figures, including the phase seconds and the distance flown. `receiverLatLon` may be null when the
// receiver's position is not known, and then the range fields stay unset.
void summarise(Flight& f, const std::vector<Point>& pts, const double* receiverLatLon);

// Where the flight began and ended, from the track and the airport table. Only names an airport within radiusNm, and only one that has a
// runway long enough for the aircraft to have used it (minRunwayFt, 0 for any).
void findEnds(Flight& f, const std::vector<Point>& pts, const AirportDb& db, float radiusNm = 8, int minRunwayFt = 0);

// Measures the arrival against the runway it looks like it used. Returns false when the track never descended towards a known runway.
bool fitApproach(const std::vector<Point>& pts, const AirportDb& db, ApproachFit& out, float glideDeg = 3.0f);

// ---------------------------------------------------------------- the path, and whether it is the usual one

// A short string standing for the path flown: the 0.25 degree cells the track passed through, in order, without repeats. Two flights with
// the same signature followed the same path at that resolution. Stable across runs, so it can be compared in the store.
std::string routeSignature(const std::vector<Point>& pts, double cellDeg = 0.25);

// How far apart two tracks are, in nautical miles: the mean over the points of `a` of the distance to the nearest leg of `b`, and the same
// the other way, whichever is larger. 0 means the same ground path. Used for "is this the route it usually flies".
float trackDistanceNm(const std::vector<Point>& a, const std::vector<Point>& b);

// ---------------------------------------------------------------- what kind of operation this is

enum OperationKind : uint8_t {
    OpUnknown = 0,
    OpAirline,        // a scheduled or charter flight under an airline call sign
    OpCargo,
    OpPrivate,        // flying under its own registration
    OpMedical,        // air ambulance, including hospital helicopters
    OpMilitary,
    OpSurvey,         // survey, patrol, pipeline, traffic watch: long low passes in a pattern
    OpSkydive,        // climbs to altitude over one spot and comes straight down, repeatedly
    OpTraining,       // circuits at one airport
    OpKindCount
};
const char* operationKindName(OperationKind k);

// What is known about the aircraft itself when the kind is inferred (from AdsbRegDb and the ADS-B emitter category).
struct AircraftFacts {
    uint32_t icao = 0;
    std::string registration, type, operatorName;
    std::string category;      // ADS-B emitter category, "A3", "A7" (rotorcraft), "B1" (glider) ...
    bool rotorcraft = false;   // category A7
};

struct KindGuess {
    OperationKind kind = OpUnknown;
    float confidence = 0;
    std::string basis;
};

// Infers the kind from the call sign, the operator name, the address block and the shape of the track. Deliberately conservative: it
// returns OpUnknown with confidence 0 rather than guess from nothing, and the basis always says what it used.
KindGuess inferKind(const Flight& f, const std::vector<Point>& pts, const AircraftFacts& facts);

// true when the address is in a block a national air force is known to use (the US military range, and the NATO / unallocated-use blocks
// that aircraft without a civil registration appear in). A military aircraft can fly on a civil address, and a civil aircraft can be in
// one of these blocks, so this is a hint and never a conclusion.
bool militaryAddressBlock(uint32_t icao);

// The call sign of an airline flight: three letters of an ICAO operator code then one to four digits, optionally a letter ("DAL123",
// "BAW22AB"). Fills `op` with the three letters.
bool airlineCallsign(const std::string& callsign, std::string& op);

// A call sign that is just the aircraft's own registration (an N-number flying as "N123AB"), which marks a private flight.
bool callsignIsRegistration(const std::string& callsign, const std::string& registration);

// Words that appear in the call signs and operator names of air ambulance and police / rescue operations, and of the military. Public
// lists; the match is reported in the basis so the user can disagree with it.
bool medicalWord(const std::string& s);
bool militaryWord(const std::string& s);
bool surveyWord(const std::string& s);

// Does the track climb to altitude over one spot and come straight down again? Counts the cycles; two or more is the signature of a
// parachute operation (the aircraft climbs, the jumpers leave, it descends fast and lands).
int climbDropCycles(const std::vector<Point>& pts, int minClimbFt = 4000, float withinNm = 6);

// Circuits at one airport: how many times the track came down to the ground (or near it) and climbed away again at the same place.
int circuitCount(const std::vector<Point>& pts, float withinNm = 3);

// ---------------------------------------------------------------- the segmenter

struct SegmenterOptions {
    double gapSec = 300;                 // silence this long ends the flight
    double groundSplitSec = 600;         // this long on the ground, with no flying after it, also ends it
    double minFlightSec = 20;            // shorter than this is dropped as noise
    size_t minPoints = 3;
    size_t maxPoints = 20000;            // a very long flight stops recording points (the figures keep going)
    double pointMinSec = 1.0;            // positions closer together than this are thinned out
    double pointMinMoveNm = 0.02;        // ... unless the aircraft moved this far
};

// Feeds aircraft records in and hands finished flights back. One instance follows every aircraft at once; the recorder calls feed() with
// whatever the telemetry holds and takes whatever has finished. Times are Unix seconds and must not go backwards.
class Segmenter {
public:
    explicit Segmenter(const SegmenterOptions& o = SegmenterOptions());
    ~Segmenter();

    // One aircraft as the telemetry has it now. `t` is the wall-clock time of this observation. Only a record that carries something new
    // (a position, or kinematics) adds a point; the rest updates the flight's identity and counters.
    void feed(const AdsbAircraft& a, double t);
    // Same, for a track read from elsewhere (a test, or an imported log).
    void feedPoint(uint32_t icao, const Point& p, const std::string& callsign = "", const std::string& squawk = "");

    // Flights that have ended (silence, or a landing followed by a long stop). Each comes with its points. The caller takes them away.
    struct Finished { Flight f; std::vector<Point> pts; };
    std::vector<Finished> take(double now);
    // Ends every open flight, whether or not it has been silent: at shutdown, or when the receiver stops.
    std::vector<Finished> flush();

    size_t openFlights() const;
    // The flight being recorded for an aircraft right now, for the live screens; nullptr when none.
    const Flight* open(uint32_t icao) const;
    const std::vector<Point>* openPoints(uint32_t icao) const;

    // Given the airport table, a finished flight is completed fully: the field elevation for the phases, the approach phase, where it
    // began and ended and the approach measurement. Without it a flight still gets its phases and figures, but no airports.
    void setAirports(const AirportDb* db, float endRadiusNm = 8, int minRunwayFt = 0);
    void setReceiver(double lat, double lon);   // so ranges and the ends can be worked out
    void clearReceiver();

private:
    struct Impl;
    Impl* p_;
};

} // namespace dect2::flights
