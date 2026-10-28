#include "station_loc.h"
#include "../app.h"
#include <cmath>
#include <cstdio>

using dect2::geo::StationPos;

namespace {

struct Station {
    bool loaded = false;
    StationPos pos;          // invalid while unset
    unsigned gen = 1;
    std::string envErr;
};
Station G;

constexpr double kDefaultLat = 25.25, kDefaultLon = 55.36;   // the placeholder the maps used before a position was set
constexpr double kUnknown = -9999.0;                         // stored for "not known": prefs have no delete, and this is outside every valid altitude

void load() {
    if (G.loaded) return;
    G.loaded = true;
    plat::Prefs& d = plat::prefs();
    dect2::geo::StoredStation s;
    s.hasLat = d.has("adsbLat"); s.hasLon = d.has("adsbLon");
    s.lat = d.getD("adsbLat", 0); s.lon = d.getD("adsbLon", 0);
    s.hasAlt = d.has("stationAltM"); s.altM = d.getD("stationAltM", 0);
    s.hasOldAlt = d.has("rxAltM"); s.oldAltM = d.getD("rxAltM", 0);
    s.hasAnt = d.has("stationAntM"); s.antM = d.getD("stationAntM", 0);
    s.source = d.getS("stationLocSource", "");
    G.pos = dect2::geo::stationFromStored(s);
    if (G.pos.valid()) return;   // a saved position always wins
    std::string err;
    const StationPos e = dect2::geo::envLocation(&err);
    if (e.valid()) { G.pos = e; return; }   // in memory only
    if (!err.empty()) {
        G.envErr = err;
        std::fprintf(stderr, "My station: the location from the environment is ignored: %s\n", err.c_str());
    }
}

bool fail(std::string* err, const char* why) { if (err) *err = why; return false; }

}   // namespace

dect2::geo::StationPos stationPos() { load(); return G.pos; }
unsigned stationGeneration() { load(); return G.gen; }
std::string stationEnvError() { load(); return G.envErr; }
std::string stationSource() { load(); return G.pos.valid() ? G.pos.source : std::string(); }
double stationAntennaM() { load(); return G.pos.valid() ? G.pos.antM : NAN; }

void receiverLocation(double& lat, double& lon) {
    load();
    if (G.pos.valid()) { lat = G.pos.lat; lon = G.pos.lon; }
    else { lat = kDefaultLat; lon = kDefaultLon; }
}

bool receiverLocationSet() { load(); return G.pos.valid(); }

double receiverAltitudeM() { load(); return G.pos.valid() && !std::isnan(G.pos.altM) ? G.pos.altM : 0.0; }

bool setStationPos(App& a, const StationPos& in, std::string* err) {
    load();
    if (!dect2::geo::validPos(in.lat, in.lon)) return fail(err, "latitude must be -90 to 90 and longitude -180 to 180 (and not both 0)");
    if (!std::isnan(in.altM) && !dect2::geo::validAlt(in.altM)) return fail(err, "height above sea level must be -500 to 9000 m");
    if (!std::isnan(in.antM) && !(in.antM >= 0 && in.antM <= 1000)) return fail(err, "antenna height must be 0 to 1000 m");
    StationPos n = in;
    if (n.source.empty()) n.source = "typed";
    if (std::isnan(n.antM) && G.pos.valid()) n.antM = G.pos.antM;
    if (std::isnan(n.altM) && G.pos.valid() && dect2::geo::distanceM(G.pos.lat, G.pos.lon, n.lat, n.lon) < 5000.0) n.altM = G.pos.altM;
    G.pos = n;
    G.gen++;
    plat::Prefs& d = plat::prefs();
    d.setD("adsbLat", n.lat); d.setD("adsbLon", n.lon);
    d.setD("stationAltM", std::isnan(n.altM) ? kUnknown : n.altM);
    d.setD("rxAltM", std::isnan(n.altM) ? 0.0 : n.altM);   // the old key, kept in step so an older version shows the same height
    d.setD("stationAntM", std::isnan(n.antM) ? kUnknown : n.antM);
    d.setS("stationLocSource", n.source);
    savePrefs(a);
    adsbui::onStationChanged(a);
    return true;
}

void setReceiverLocation(App& a, double lat, double lon, const char* source) {
    StationPos p;
    p.lat = lat; p.lon = lon; p.source = source ? source : "map";
    setStationPos(a, p);
}

std::string stationSourceLabel(const std::string& s) {
    if (s == "typed" || s == "manual") return tr("typed");
    if (s == "pasted") return tr("pasted");
    if (s == "locator") return tr("locator");
    if (s == "map") return tr("map");
    if (s == "gps") return tr("GPS");
    if (s == "wifi") return tr("Wi-Fi positioning");
    if (s == "env") return tr("from the container (env)");
    if (s == "legacy") return tr("from earlier ADS-B setting");
    return s;
}
