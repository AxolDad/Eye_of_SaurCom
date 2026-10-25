// UAT (978 MHz) telemetry for the receiver and UI.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct UatAircraft {
    uint32_t icao = 0;
    std::string callsign;
    bool hasSquawk = false;
    int squawk = 0;
    bool hasAlt = false;
    int altFt = 0;
    bool hasGeoAlt = false;
    int geoAltFt = 0;
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasSpeed = false;
    int speedKt = 0;
    bool hasHeading = false;
    float headingDeg = 0;
    bool hasVrate = false;
    int vrateFpm = 0;
    bool hasRange = false;
    float distNm = 0, bearingDeg = 0;
    bool ground = false;
    bool tisb = false;
    int nic = -1;
    uint32_t messages = 0;
    float ageSec = 0;
    float levelDbfs = -120;
    std::vector<std::pair<float, float>> track;   // lat, lon
};

// One FIS-B product as the ground station last broadcast it (fisb.h). Products are broadcast again and again: a repeat updates the one
// already held rather than adding another.
struct UatFisbProduct {
    uint16_t productId = 0;
    std::string title;           // what it is: "METAR", "TAF", "PIREP", "NOTAM-D", "NOTAM-TFR", "AIRMET" ...
    std::string text;
    double timeSec = 0;          // when it was last heard (signal time)
    double firstSec = 0;         // when it was first heard
    uint32_t heard = 0;          // how many times it has been broadcast since
    // Identity, for products that carry it (the TWGO ones: NOTAMs, AIRMETs, SIGMETs ...). DO-358B B.3.3: the product, the report's year
    // and number, the location and the month of the broadcast together name one report.
    std::string location;        // an airport for a NOTAM-D or -FDC; empty for a NOTAM-TFR
    uint16_t reportNumber = 0;
    uint8_t reportYear = 0;
    bool cancelled = false;
    // The graphic part, where one came: the airspace a TFR, an AIRMET or a SIGMET covers.
    struct Area {
        std::vector<std::pair<double, double>> ring;   // lat, lon of a polygon
        double centreLat = 0, centreLon = 0, radiusNm = 0;   // a circle, when ring is empty
        int floorFt = 0, ceilingFt = 0;
        bool agl = false;        // the altitudes are above ground rather than above sea level
    };
    std::vector<Area> areas;
};

struct UatFrameInfo {
    std::string hex;
    std::string kind;            // "ADS-B", "TIS-B", "FIS-B", "UAT"
    uint32_t address = 0;
    double timeSec = 0;
    float levelDbfs = 0;
    std::string summary;
};

struct UatTelemetry {
    uint64_t seq = 0;
    int state = 0;
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;

    double timeSec = 0;
    float msgsPerSec = 0;
    float noiseDbfs = -120;
    uint64_t frameCount = 0;
    uint32_t aircraftCount = 0;
    uint32_t withPosition = 0;
    uint32_t fisbCount = 0;
    uint32_t tisbCount = 0;
    uint32_t otherCount = 0;
    bool refValid = false;
    double refLat = 0, refLon = 0;
    std::vector<UatAircraft> aircraft;
    std::vector<UatFisbProduct> fisb;
    std::vector<UatFrameInfo> framesLog;
};

inline std::string uatSummary(const UatTelemetry& t) {
    char b[220];
    snprintf(b, sizeof b, "UAT: state %d  %.0f msg/s  %u aircraft (%u pos)  FIS-B %u  TIS-B %u  SNR %.1f dB  ok %llu  bad %llu",
             t.state, t.msgsPerSec, t.aircraftCount, t.withPosition, t.fisbCount, t.tisbCount, t.snrDb,
             (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
