// Deep Space Network Now (dsn.h): the Rust crate's tables turned into a tree.
#include "dect2/net/dsn.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/dsn.h"
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace dsn {

namespace {
struct AbiCheck {
    AbiCheck() { rsAbiCheck("onair_dsn", {onair_dsn_feed_size(), onair_dsn_config_size()}, {sizeof(OnairDsnFeed), sizeof(OnairDsnConfig)}); }
} abiCheck;

const uint8_t* u8(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }
} // namespace

Names::Names() : c_(new OnairDsnConfig()) {}
Names::~Names() = default;

bool Names::load(const std::string& xml) {
    auto c = std::make_unique<OnairDsnConfig>();
    if (!onair_dsn_parse_config(u8(xml), xml.size(), c.get())) return false;
    c_ = std::move(c);
    loaded_ = true;
    return true;
}

std::string Names::spacecraft(const std::string& call) const {
    if (!loaded_) return "";
    char b[80];
    return onair_dsn_spacecraft_name(c_.get(), u8(call), call.size(), reinterpret_cast<uint8_t*>(b), sizeof b) ? std::string(b) : "";
}

std::string Names::dishType(const std::string& dish) const {
    if (!loaded_) return "";
    const int i = onair_dsn_dish_index(c_.get(), u8(dish), dish.size());
    return i >= 0 ? std::string(c_->dishes[i].kind) : "";
}

bool Names::site(const std::string& name, double& lat, double& lon) const {
    if (!loaded_) return false;
    for (uint32_t i = 0; i < c_->n_sites && i < ONAIR_DSN_MAX_SITES; i++)
        if (name == c_->sites[i].name) { lat = c_->sites[i].latitude; lon = c_->sites[i].longitude; return true; }
    return false;
}

size_t Names::spacecraftCount() const { return loaded_ ? c_->n_spacecraft : 0; }

bool parseReport(const std::string& xml, const Names* names, Report& out) {
    out = Report();
    auto f = std::make_unique<OnairDsnFeed>();
    if (!onair_dsn_parse(u8(xml), xml.size(), f.get())) return false;
    out.timestampMs = f->timestamp_ms;
    for (uint32_t i = 0; i < f->n_stations; i++) {
        Station s;
        s.name = f->stations[i].name; s.friendly = f->stations[i].friendly;
        s.utcMs = f->stations[i].time_utc_ms; s.tzOffsetMs = f->stations[i].tz_offset_ms;
        s.lat = s.lon = NAN;
        if (names) names->site(s.name, s.lat, s.lon);
        out.stations.push_back(std::move(s));
    }
    if (out.stations.empty()) { out.stations.emplace_back(); out.stations.back().friendly = "?"; out.stations.back().lat = out.stations.back().lon = NAN; }
    std::vector<std::pair<int, int>> where(f->n_dishes);   // dish index -> (station, position in it)
    for (uint32_t i = 0; i < f->n_dishes; i++) {
        const OnairDsnDish& r = f->dishes[i];
        Dish d;
        d.name = r.name; d.activity = r.activity;
        if (names) d.type = names->dishType(d.name);
        d.azDeg = r.azimuth_deg; d.elDeg = r.elevation_deg; d.windKmh = r.wind_kmh;
        d.mspa = r.mspa; d.array = r.array; d.ddor = r.ddor;
        const int st = r.station >= 0 && r.station < (int)out.stations.size() ? r.station : 0;
        where[i] = {st, (int)out.stations[st].dishes.size()};
        out.stations[st].dishes.push_back(std::move(d));
    }
    for (uint32_t i = 0; i < f->n_targets; i++) {
        const OnairDsnTarget& r = f->targets[i];
        if (r.dish < 0 || r.dish >= (int)where.size()) continue;
        Target t;
        t.call = r.name; t.id = r.id;
        if (names) t.name = names->spacecraft(t.call);
        t.uplegKm = r.upleg_km; t.downlegKm = r.downleg_km; t.rtltS = r.rtlt_s;
        out.stations[where[r.dish].first].dishes[where[r.dish].second].targets.push_back(std::move(t));
    }
    for (uint32_t i = 0; i < f->n_signals; i++) {
        const OnairDsnSignal& r = f->signals[i];
        if (r.dish < 0 || r.dish >= (int)where.size()) continue;
        Signal s;
        s.spacecraft = r.spacecraft; s.up = r.up; s.active = r.active; s.kind = r.kind; s.band = (char)r.band;
        s.rateBps = r.data_rate_bps; s.freqHz = r.frequency_hz; s.power = r.power; s.powerFlagged = r.power_flagged;
        out.stations[where[r.dish].first].dishes[where[r.dish].second].signals.push_back(std::move(s));
    }
    return true;
}

const char* bandName(char b) { return b == 'S' ? "S" : b == 'X' ? "X" : b == 'K' ? "Ka" : b == 'L' ? "L" : b == 'U' ? "UHF" : "?"; }

const char* kindName(int k) { return k == 1 ? "carrier" : k == 2 ? "data" : k == 3 ? "other" : "idle"; }

std::vector<Allocation> allocations(char band, bool down) {
    switch (band) {
    case 'S': return down ? std::vector<Allocation>{{"near Earth", 2200, 2290}, {"deep space", 2290, 2300}}
                          : std::vector<Allocation>{{"near Earth", 2025, 2110}, {"deep space", 2110, 2120}};
    case 'X': return down ? std::vector<Allocation>{{"deep space", 8400, 8450}, {"near Earth", 8450, 8500}}
                          : std::vector<Allocation>{{"deep space", 7145, 7190}, {"near Earth", 7190, 7235}};
    case 'K': return down ? std::vector<Allocation>{{"near Earth", 25500, 27000}, {"deep space", 31800, 32300}}
                          : std::vector<Allocation>{{"deep space", 34200, 34700}};
    default: return {};
    }
}

std::string distanceText(double km) {
    char b[64];
    if (!std::isfinite(km)) return "-";
    if (km < 1e6) snprintf(b, sizeof b, "%.0f km", km);
    else if (km < 1e9) snprintf(b, sizeof b, "%.2f million km (%.3f AU)", km / 1e6, km / 149597870.7);
    else snprintf(b, sizeof b, "%.2f billion km (%.1f AU)", km / 1e9, km / 149597870.7);
    return b;
}

std::string lightTimeText(double s) {
    char b[48];
    if (!std::isfinite(s)) return "-";
    if (s < 120) snprintf(b, sizeof b, "%.1f s", s);
    else if (s < 7200) snprintf(b, sizeof b, "%.1f min", s / 60);
    else snprintf(b, sizeof b, "%.2f h", s / 3600);
    return b;
}

std::string rateText(double bps) {
    char b[32];
    if (bps <= 0) return "-";
    if (bps < 1e3) snprintf(b, sizeof b, "%.4g bit/s", bps);
    else if (bps < 1e6) snprintf(b, sizeof b, "%.4g kbit/s", bps / 1e3);
    else snprintf(b, sizeof b, "%.4g Mbit/s", bps / 1e6);
    return b;
}

} // namespace dsn
} // namespace dect2
