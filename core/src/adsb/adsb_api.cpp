// Aircraft from an aggregator's web API: see adsb_api.h
#include "dect2/adsb/adsb_api.h"
#include "dect2/automation/json_mini.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace {

namespace json = dect2::automation::json;

constexpr double kPi = 3.14159265358979323846;
constexpr double kNmPerDegLat = 60.0;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

// A 24-bit address written as hex, as every one of these services writes it. Returns 0 for anything else, including the "~" prefix those
// services put on a non-ICAO (TIS-B) address, which this program keeps apart from real addresses anyway.
uint32_t hexAddr(const std::string& s) {
    const std::string t = trim(s);
    if (t.empty() || t.size() > 6) return 0;
    uint32_t v = 0;
    for (char c : t) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = v * 16 + (uint32_t)d;
    }
    return v & 0xFFFFFF;
}

// These services send a number either as a number or as a string, and alt_baro is the word "ground" when the aircraft is on it.
bool numOf(const json::Value& o, const char* key, double& out) {
    const json::Value* v = o.get(key);
    if (!v) return false;
    if (v->type == json::Value::Num) { out = v->n; return true; }
    if (v->type == json::Value::Str) {
        const std::string t = trim(v->s);
        if (t.empty()) return false;
        char* end = nullptr;
        const double d = std::strtod(t.c_str(), &end);
        if (end == t.c_str()) return false;
        out = d;
        return true;
    }
    return false;
}

std::string strOf(const json::Value& o, const char* key) {
    const json::Value* v = o.get(key);
    return v && v->type == json::Value::Str ? trim(v->s) : std::string();
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t i = s.find(from); i != std::string::npos; i = s.find(from, i + to.size())) s.replace(i, from.size(), to);
}

std::string fmt(double v, int dp) {
    char b[48];
    snprintf(b, sizeof b, "%.*f", dp, v);
    return b;
}

}  // namespace

// ---------------------------------------------------------------- the services

const std::vector<ApiService>& apiServices() {
    static const std::vector<ApiService> v = {
        // Verified against the live service: returns {"ac":[...]} with readsb field names, no key.
        {"adsblol", "adsb.lol", ApiShape::ReadsbJson, "https://api.adsb.lol/v2/point/{lat}/{lon}/{radiusNm}", "ac",
         ApiKeyMode::None, 5, 250, true, "Community network, no key. Be gentle: it is run by volunteers.", "https://api.adsb.lol/docs"},
        // Verified against the live service: {"aircraft":[...]}, same fields, different URL shape and array key.
        {"adsbfi", "adsb.fi", ApiShape::ReadsbJson, "https://opendata.adsb.fi/api/v2/lat/{lat}/lon/{lon}/dist/{radiusNm}", "aircraft",
         ApiKeyMode::None, 5, 250, true, "Community network, no key.", "https://github.com/adsbfi/opendata"},
        // Verified against the live service: positional arrays. Anonymous use has a small daily allowance; an account raises it and goes in
        // the address as user:password, which is the only way a key can reach it here.
        {"opensky", "OpenSky Network", ApiShape::OpenSkyStates,
         "https://{key}opensky-network.org/api/states/all?lamin={latMin}&lomin={lonMin}&lamax={latMax}&lomax={lonMax}", "states",
         ApiKeyMode::UserInfo, 10, 250, true,
         "Works without an account, with a small daily allowance. A free account raises it: enter it as user:password.",
         "https://openskynetwork.github.io/opensky-api/rest.html"},
        // Their API answered 403 with "contact us at contact@airplanes.live ... include a description of the project": they decide who may
        // use it. Feeding TO them needs nothing of the sort and is in the Send to list.
        {"airplaneslive", "airplanes.live", ApiShape::ReadsbJson, "https://api.airplanes.live/v2/point/{lat}/{lon}/{radiusNm}", "ac",
         ApiKeyMode::None, 5, 250, false,
         "Their API refuses unknown callers: email contact@airplanes.live describing your project to be let in. Feeding to them needs no "
         "permission and is in the Send to list.",
         "https://airplanes.live/api-guide/"},
        // Paid, and the token must go in an Authorization header, which this program's fetch helper cannot send yet.
        {"fr24", "Flightradar24", ApiShape::ReadsbJson, "", "data", ApiKeyMode::NeedsHeader, 10, 250, false,
         "Their API is a paid subscription and wants the token in a request header, which this program cannot send yet. Feeding to "
         "Flightradar24 works through the fr24feed program you run yourself: see the Send to list.",
         "https://fr24api.flightradar24.com/"},
        {"adsbexchange", "ADSB Exchange", ApiShape::ReadsbJson, "", "ac", ApiKeyMode::NeedsHeader, 10, 250, false,
         "Their API is paid, through RapidAPI, and wants the key in a request header, which this program cannot send yet. Feeding to them "
         "is in the Send to list.",
         "https://www.adsbexchange.com/data/"},
    };
    return v;
}

const ApiService* apiServiceById(const std::string& id) {
    for (const ApiService& s : apiServices())
        if (id == s.id) return &s;
    return nullptr;
}

// ---------------------------------------------------------------- the URL

bool apiKeyLooksSafe(const std::string& key) {
    if (key.size() > 200) return false;
    for (char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                        c == '~' || c == ':';
        if (!ok) return false;
    }
    return true;
}

void apiBoundingBox(double latDeg, double lonDeg, double radiusNm, double& latMin, double& lonMin, double& latMax, double& lonMax) {
    const double dLat = radiusNm / kNmPerDegLat;
    // The circle is wider in longitude the nearer the poles; at the pole itself it is the whole way round.
    const double cosLat = std::cos(latDeg * kPi / 180.0);
    const double dLon = std::fabs(cosLat) < 1e-6 ? 180.0 : std::min(180.0, radiusNm / (kNmPerDegLat * std::fabs(cosLat)));
    latMin = std::max(-90.0, latDeg - dLat);
    latMax = std::min(90.0, latDeg + dLat);
    lonMin = lonDeg - dLon;
    lonMax = lonDeg + dLon;
    // Past the date line, ask for the whole band rather than send a box that reads inside out: these services take it literally.
    if (lonMin < -180.0 || lonMax > 180.0) { lonMin = -180.0; lonMax = 180.0; }
}

std::string apiUrl(const ApiService& s, double latDeg, double lonDeg, double radiusNm, const std::string& key, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return std::string(); };
    if (s.keyMode == ApiKeyMode::NeedsHeader || !s.urlTemplate || !*s.urlTemplate)
        return fail("this program cannot reach that service yet");
    if (!(latDeg >= -90 && latDeg <= 90) || !(lonDeg >= -180 && lonDeg <= 180)) return fail("no receiver position");
    if (!apiKeyLooksSafe(key)) return fail("the key has characters that cannot go in an address");
    if ((s.keyMode == ApiKeyMode::Query) && key.empty()) return fail("this service needs a key");

    const double r = std::max(1.0, std::min(radiusNm, s.maxRadiusNm));
    double latMin, lonMin, latMax, lonMax;
    apiBoundingBox(latDeg, lonDeg, r, latMin, lonMin, latMax, lonMax);

    std::string u = s.urlTemplate;
    replaceAll(u, "{lat}", fmt(latDeg, 5));
    replaceAll(u, "{lon}", fmt(lonDeg, 5));
    replaceAll(u, "{radiusNm}", fmt(r, 0));
    replaceAll(u, "{latMin}", fmt(latMin, 5));
    replaceAll(u, "{lonMin}", fmt(lonMin, 5));
    replaceAll(u, "{latMax}", fmt(latMax, 5));
    replaceAll(u, "{lonMax}", fmt(lonMax, 5));
    // UserInfo: the key sits in front of the host as user:password@, and nothing at all when there is no key.
    if (s.keyMode == ApiKeyMode::UserInfo) replaceAll(u, "{key}", key.empty() ? "" : key + "@");
    else replaceAll(u, "{key}", key);
    return u;
}

// ---------------------------------------------------------------- the readers

namespace {

// One aircraft of a readsb-style response.
bool readReadsbAircraft(const json::Value& o, ApiAircraft& a) {
    if (o.type != json::Value::Obj) return false;
    a.icao = hexAddr(strOf(o, "hex"));
    if (!a.icao) return false;
    a.callsign = strOf(o, "flight");
    a.registration = strOf(o, "r");
    a.type = strOf(o, "t");
    a.category = strOf(o, "category");
    double n = 0;
    if (numOf(o, "lat", n)) {
        a.lat = n;
        if (numOf(o, "lon", n)) { a.lon = n; a.hasPos = a.lat >= -90 && a.lat <= 90 && a.lon >= -180 && a.lon <= 180; }
    }
    // alt_baro is the word "ground" when it is on the ground, a number when it is not.
    if (const json::Value* alt = o.get("alt_baro")) {
        if (alt->type == json::Value::Str && trim(alt->s) == "ground") a.ground = true;
        else if (numOf(o, "alt_baro", n)) { a.hasAlt = true; a.altFt = (int)std::lround(n); }
    }
    if (!a.hasAlt && !a.ground && numOf(o, "alt_geom", n)) { a.hasAlt = true; a.altFt = (int)std::lround(n); }
    if (numOf(o, "gs", n)) { a.hasSpeed = true; a.speedKt = n; }
    if (numOf(o, "track", n)) { a.hasHeading = true; a.headingDeg = n; }
    else if (numOf(o, "true_heading", n)) { a.hasHeading = true; a.headingDeg = n; }
    if (numOf(o, "baro_rate", n)) { a.hasVrate = true; a.vrateFpm = (int)std::lround(n); }
    else if (numOf(o, "geom_rate", n)) { a.hasVrate = true; a.vrateFpm = (int)std::lround(n); }
    // The squawk is four octal digits written as text ("2004"); this program keeps it as those digits read as a decimal number.
    const std::string sq = strOf(o, "squawk");
    if (sq.size() == 4 && sq.find_first_not_of("01234567") == std::string::npos) {
        a.hasSquawk = true;
        a.squawk = std::atoi(sq.c_str());
    }
    const std::string em = strOf(o, "emergency");
    a.emergency = !em.empty() && em != "none";
    if (numOf(o, "seen", n)) a.seenSec = n;
    return true;
}

// OpenSky's documented order. Anything shorter than this is not a state vector.
// 0 icao24, 1 callsign, 2 origin_country, 3 time_position, 4 last_contact, 5 longitude, 6 latitude, 7 baro_altitude, 8 on_ground,
// 9 velocity (m/s), 10 true_track, 11 vertical_rate (m/s), 12 sensors, 13 geo_altitude, 14 squawk, 15 spi, 16 position_source
constexpr size_t kOpenSkyMinFields = 17;

bool readOpenSkyState(const json::Value& row, double now, ApiAircraft& a) {
    if (row.type != json::Value::Arr || row.a.size() < kOpenSkyMinFields) return false;
    auto num = [&](size_t i, double& out) {
        if (i >= row.a.size() || row.a[i].type != json::Value::Num) return false;
        out = row.a[i].n;
        return true;
    };
    auto str = [&](size_t i) { return i < row.a.size() && row.a[i].type == json::Value::Str ? trim(row.a[i].s) : std::string(); };
    a.icao = hexAddr(str(0));
    if (!a.icao) return false;
    a.callsign = str(1);
    double n = 0;
    if (num(6, n)) {
        a.lat = n;
        if (num(5, n)) { a.lon = n; a.hasPos = a.lat >= -90 && a.lat <= 90 && a.lon >= -180 && a.lon <= 180; }
    }
    a.ground = 8 < row.a.size() && row.a[8].type == json::Value::Bool && row.a[8].b;
    // OpenSky sends metres and metres per second; this program works in feet and knots.
    if (num(7, n)) { a.hasAlt = true; a.altFt = (int)std::lround(n * 3.28084); }
    else if (num(13, n)) { a.hasAlt = true; a.altFt = (int)std::lround(n * 3.28084); }
    if (num(9, n)) { a.hasSpeed = true; a.speedKt = n * 1.9438445; }
    if (num(10, n)) { a.hasHeading = true; a.headingDeg = n; }
    if (num(11, n)) { a.hasVrate = true; a.vrateFpm = (int)std::lround(n * 196.850394); }
    const std::string sq = str(14);
    if (sq.size() == 4 && sq.find_first_not_of("01234567") == std::string::npos) {
        a.hasSquawk = true;
        a.squawk = std::atoi(sq.c_str());
    }
    if (num(4, n) && now > 0 && n > 0) a.seenSec = std::max(0.0, now - n);
    return true;
}

}  // namespace

bool parseApiResponse(ApiShape shape, const std::string& body, const std::string& arrayKey, std::vector<ApiAircraft>& out,
                      std::string* err, size_t* skipped) {
    out.clear();
    if (skipped) *skipped = 0;
    if (body.empty()) { if (err) *err = "the service sent nothing"; return false; }
    json::Value root;
    std::string jerr;
    if (!json::parse(body, root, jerr)) {
        // A response bigger than the reader's limit lands here too, which is why the radius is capped.
        if (err) *err = "could not read the answer: " + jerr;
        return false;
    }
    if (root.type != json::Value::Obj) { if (err) *err = "the answer is not an object"; return false; }
    // A service that refuses tends to answer with an object carrying nothing but a message; say so rather than report no aircraft.
    if (const json::Value* e = root.get("error")) {
        if (e->type == json::Value::Str) { if (err) *err = "the service refused: " + e->s; return false; }
    }

    if (shape == ApiShape::OpenSkyStates) {
        const double now = root.num("time", 0);
        const json::Value* arr = root.get(arrayKey.empty() ? "states" : arrayKey);
        if (!arr || arr->type != json::Value::Arr) {
            // No states at all is what OpenSky sends for an empty box, as null; that is an answer, not a fault.
            if (arr && arr->type == json::Value::Null) return true;
            if (err) *err = "the answer has no states";
            return false;
        }
        for (const json::Value& row : arr->a) {
            ApiAircraft a;
            if (readOpenSkyState(row, now, a)) out.push_back(std::move(a));
            else if (skipped) (*skipped)++;
        }
        return true;
    }

    // readsb-style: try the service's own key, then the two names these services actually use, so one preset with the wrong key still works.
    const json::Value* arr = root.get(arrayKey);
    if (!arr || arr->type != json::Value::Arr) arr = root.get("ac");
    if (!arr || arr->type != json::Value::Arr) arr = root.get("aircraft");
    if (!arr || arr->type != json::Value::Arr) {
        if (err) *err = "the answer has no aircraft list";
        return false;
    }
    for (const json::Value& o : arr->a) {
        ApiAircraft a;
        if (readReadsbAircraft(o, a)) out.push_back(std::move(a));
        else if (skipped) (*skipped)++;
    }
    return true;
}

// ---------------------------------------------------------------- into the table

AdsbExtUpdate apiToExtUpdate(const ApiAircraft& a) {
    AdsbExtUpdate u;
    u.icao = a.icao;
    u.callsign = a.callsign;
    if (a.hasAlt) { u.hasAlt = true; u.altFt = a.altFt; }
    if (a.hasSpeed) { u.hasSpeed = true; u.speedKt = a.speedKt; }
    if (a.hasHeading) { u.hasHeading = true; u.headingDeg = a.headingDeg; }
    if (a.hasVrate) { u.hasVrate = true; u.vrateFpm = a.vrateFpm; }
    if (a.hasPos) { u.hasPos = true; u.lat = a.lat; u.lon = a.lon; }
    if (a.hasSquawk) { u.hasSquawk = true; u.squawk = a.squawk; }
    u.hasGround = true;
    u.ground = a.ground;
    return u;
}

}  // namespace dect2
