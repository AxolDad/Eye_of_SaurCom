// Airport table: ICAO / IATA code, name, city, country, position, the radio frequencies of each airport (tower, ground, ATIS, approach ...)
// and its runways.
//
// The built-in table is data/airports/airports.tsv, built by tools/gen_airports.py from OurAirports (public domain, https://ourairports.com/data/)
// and compiled in as text. A newer copy can be imported from the two OurAirports CSV files (importOurAirports), which the app keeps in the
// user's settings folder and prefers to the built-in one.
//
// Runways are a second built-in file, data/airports/runways.tsv, built by tools/gen_runways.py from OurAirports runways.csv: only the runways
// of the airports in airports.tsv are kept (an approach can only be measured against an airport the program knows). They are loaded into the
// same table by parseRunwaysTsv() / importRunways(), so Airport::runways carries them; the flight recorder measures approaches against them
// (core/include/dect2/flights/flights_proto.h).
//
// Frequencies are whole kHz: 118500 is the 25 kHz channel 118.500; 118505 is the 8.33 kHz channel named 118.505 (the channel 118.500 MHz with
// 8.33 spacing), 118510 is 118.50833 MHz. The same convention as the airband mode's channel names (airband_rx.h, airbandParse).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "dect2/airband/airband_rx.h"

namespace dect2 {

struct AirportFreq {
    std::string type;        // TWR GND ATIS APP DEP CTAF UNICOM CLD AFIS FSS AWOS ASOS OPS ... (upper case, as OurAirports writes them)
    std::string desc;        // the description, "" when it only repeats the type
    int khz = 0;
    bool airband() const { return khz >= 118000 && khz <= 137000; }
};

// One end of a runway: where an aircraft landing on it touches down and which way it then faces.
struct RunwayEnd {
    std::string ident;       // "04L", "22R", "H1" (a helipad), as the airport names it
    double latDeg = 0, lonDeg = 0;
    bool hasPos = false;     // OurAirports does not give a position for every threshold
    int elevFt = 0; bool hasElev = false;
    float headingDegT = 0;   // true heading of a landing on this end; from OurAirports, else the two thresholds, else the runway number
    bool hasHeading = false;
};

struct Runway {
    RunwayEnd le, he;        // the two ends, as OurAirports names them (low and high)
    int lengthFt = 0, widthFt = 0;
    bool hard = false;       // paved (asphalt, concrete, bitumen, tarmac)
    bool lit = false;
    // The end an aircraft on this true track would be landing on, or nullptr when neither is within 90 degrees of it (or has no heading).
    const RunwayEnd* endForTrack(double trackDegT) const;
};

struct Airport {
    std::string icao;        // ICAO code, or the local identifier where the airport has none
    std::string iata;
    std::string name, city, country;   // country: two letter ISO code
    double latDeg = 0, lonDeg = 0;
    int elevFt = 0;
    char kind = 'S';         // L large, M medium, S small, W seaplane base
    bool scheduled = false;  // has scheduled airline service
    std::vector<AirportFreq> freqs;
    std::vector<Runway> runways;
    size_t airbandCount() const { size_t n = 0; for (const auto& f : freqs) n += f.airband(); return n; }
    int longestRunwayFt() const { int m = 0; for (const auto& r : runways) m = r.lengthFt > m ? r.lengthFt : m; return m; }
};

struct AirportReport { size_t airports = 0, freqs = 0, skipped = 0, runways = 0; std::vector<std::string> log; };

// A frequency in MHz as typed in a data file -> whole kHz on the raster; 0 when it is not a usable radio frequency. In the airband (118 - 137 MHz)
// a value on neither the 25 kHz nor the 8.33 kHz raster is moved to the nearest 25 kHz channel when it is within 5 kHz of one, else rejected.
int airportKhz(double mhz);
std::string airportTypeName(const std::string& type);                  // "TWR" -> "Tower"
double airportDistanceKm(double lat1, double lon1, double lat2, double lon2);   // great circle

struct AirportHit { size_t index = 0; double km = -1; };   // km: distance from the position given to search / near, -1 when none was given

class AirportDb {
public:
    AirportReport parseTsv(const std::string& text);                    // replaces the contents; damaged lines are skipped and counted
    AirportReport importOurAirports(const std::string& airportsCsv, const std::string& frequenciesCsv);   // replaces the contents
    std::string toTsv() const;                                          // the table format of data/airports/airports.tsv
    // Runways into the airports already in the table (an "R" line names its airport): the format of data/airports/runways.tsv, or
    // OurAirports' runways.csv. Runways of airports not in the table are counted as skipped. Call after parseTsv / importOurAirports.
    AirportReport parseRunwaysTsv(const std::string& text);
    AirportReport importRunways(const std::string& runwaysCsv);
    std::string runwaysToTsv() const;                                   // the table format of data/airports/runways.tsv
    static const AirportDb& builtin();                                  // airports, frequencies and runways

    size_t size() const { return v_.size(); }
    bool empty() const { return v_.empty(); }
    const std::vector<Airport>& all() const { return v_; }
    const Airport* find(const std::string& code) const;                  // ICAO or IATA code, any case
    // Text search: ICAO / IATA code first (exact, then prefix), then name and city words (all words must match, as prefix of a word, then anywhere).
    // From a position, equal matches are ordered by distance; larger airports before smaller ones otherwise.
    std::vector<AirportHit> search(const std::string& text, size_t maxHits = 40, const double* fromLatLon = nullptr, bool withFreqOnly = false) const;
    // Airports within radiusKm of a position, nearest first. Only those with at least one airband frequency when airbandOnly.
    std::vector<AirportHit> nearby(double latDeg, double lonDeg, double radiusKm, size_t maxHits = 200, bool airbandOnly = true) const;
    // The nearest airport with a runway at least minRunwayFt long within radiusKm (0: any length), for naming where a flight began or ended.
    const Airport* nearestWithRunway(double latDeg, double lonDeg, double radiusKm, int minRunwayFt = 0) const;

private:
    std::vector<Airport> v_;
};

// The channels of an airport for the airband mode: every frequency in 118 - 137 MHz, once each (a frequency with two roles gets both names),
// labelled "EGLL Tower", "EGLL ATIS", "EGLL Ground 2" ...; sorted by frequency. skipped counts the frequencies outside the airband.
std::vector<AirbandChannel> airportAirbandChannels(const Airport& a, int* skipped = nullptr);

} // namespace dect2
