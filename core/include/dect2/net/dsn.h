// Deep Space Network Now: what NASA's DSN dishes at Goldstone, Madrid and Canberra are doing, from the public XML behind
// eyes.nasa.gov/apps/dsn-now (refreshed about every 5 s). The Rust crate rust/onair_dsn reads the XML; this turns its fixed tables into a
// tree of stations, dishes, targets and signals, with the spacecraft names, dish sizes and site positions of the configuration file.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct OnairDsnConfig;

namespace dect2 {
namespace dsn {

constexpr const char* kFeedUrl = "https://eyes.nasa.gov/dsn/data/dsn.xml";
constexpr const char* kConfigUrl = "https://eyes.nasa.gov/dsn/config.xml";
constexpr double kPollSec = 5.0;

struct Signal {
    std::string spacecraft;       // call sign
    bool up = false;              // uplink (dish to spacecraft)
    bool active = false;
    int kind = 0;                 // 0 none, 1 carrier, 2 data, 3 other
    char band = 0;                // 'S', 'X', 'K' (Ka), 0 unknown
    double rateBps = 0;
    double freqHz = 0;            // 0 when not published
    double power = 0;             // uplink kW, downlink dBm
    bool powerFlagged = false;    // the feed had the downlink power 300 dB low (corrected)
};

struct Target {
    std::string call;             // VGR1, JWST ...
    std::string name;             // Voyager 1 ... ("" when the configuration does not list it)
    int id = 0;
    double uplegKm = 0, downlegKm = 0, rtltS = 0;   // NaN when unknown
};

struct Dish {
    std::string name, type, activity;   // DSS43, 70M, "Spacecraft Telemetry, Tracking, and Command"
    double azDeg = 0, elDeg = 0, windKmh = 0;   // wind NaN when not given
    bool mspa = false, array = false, ddor = false;
    std::vector<Target> targets;
    std::vector<Signal> signals;
};

struct Station {
    std::string name, friendly;
    int64_t utcMs = 0, tzOffsetMs = 0;
    double lat = 0, lon = 0;      // from the configuration, NaN without it
    std::vector<Dish> dishes;
};

struct Report {
    int64_t timestampMs = 0;
    std::vector<Station> stations;
};

// The configuration file: spacecraft names, dish sizes, site positions.
class Names {
public:
    Names();
    ~Names();
    bool load(const std::string& xml);
    bool loaded() const { return loaded_; }
    std::string spacecraft(const std::string& call) const;   // "" when unknown
    std::string dishType(const std::string& dish) const;      // "" when unknown
    bool site(const std::string& name, double& lat, double& lon) const;
    size_t spacecraftCount() const;
private:
    std::unique_ptr<OnairDsnConfig> c_;
    bool loaded_ = false;
};

// Reads a report; names may be null. False when the text is not a DSN report (out is then empty).
bool parseReport(const std::string& xml, const Names* names, Report& out);

const char* bandName(char band);          // "S", "X", "Ka", "?"
const char* kindName(int kind);           // "idle", "carrier", "data", "other"

// The space research allocations of a band (ITU Radio Regulations): category A is near Earth (closer than 2 million km), B deep space.
struct Allocation { const char* use; double loMhz, hiMhz; };
std::vector<Allocation> allocations(char band, bool downlink);

// Distance as km below a million, else as millions of km and AU; light time as s, min or h.
std::string distanceText(double km);
std::string lightTimeText(double s);
std::string rateText(double bps);

} // namespace dsn
} // namespace dect2
