// Cross-site fixes: "df.bearing" events (bearing_deg, sigma_deg and the station's lat/lon) from two or more sites that are close in frequency and
// time are one transmitter seen from several places, so their bearing lines are intersected into a position fix. Thread safe: add() runs on the
// decoders' threads, fixes() on whoever shows the result.
#pragma once
#include "dect2/automation/event.h"
#include "dect2/df/position.h"
#include <mutex>
#include <string>
#include <vector>

namespace dect2::df {

struct CrossFix {
    double freqHz = 0;                 // mean frequency of the bearings used
    double time = 0;                   // time of the newest bearing used
    Fix fix;
    std::vector<std::string> sites;    // the sites whose bearings were inliers, sorted ("" = this station)
};

class CrossFixer {
public:
    static constexpr double kFreqTolHz = 25e3;   // bearings within +-25 kHz ...
    static constexpr double kTimeTolS = 5;       // ... and +-5 s of each other
    static constexpr double kMaxAgeS = 30;       // older bearings are dropped
    void add(const automation::Event& bearingEvent);          // events without a bearing or a station position are ignored
    std::vector<CrossFix> fixes(double now) const;            // one fix per group of bearings from at least two sites
private:
    struct Entry { std::string site; double time, freqHz, bearing, sigma, lat, lon; };
    mutable std::mutex mu_;
    std::vector<Entry> e_;
};

}  // namespace dect2::df
