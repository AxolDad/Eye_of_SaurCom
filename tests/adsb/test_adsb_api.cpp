// Aircraft from an aggregator's web API (core/include/dect2/adsb/adsb_api.h): the URL builder, the two readers, and the conversion into
// what the aircraft table takes.
//
// The response samples below are real answers captured from the live services, trimmed to a couple of aircraft and left otherwise exactly as
// they arrived, down to the trailing spaces in the call signs and the squawk being a string. Nothing here touches the network.
#include "dect2/adsb/adsb_api.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dect2;
#include "../common/test_util.h"

namespace {

// api.adsb.lol/v2/point/51.5/-0.12/5 - the aircraft array hangs off "ac"
const char* kAdsbLol = R"({"ac":[
{"hex":"40749b","type":"adsb_icao","flight":"BAW60   ","r":"G-ZBKR","t":"B789","alt_baro":3325,"alt_geom":3375,"gs":126.0,"ias":159,
 "tas":166,"mach":0.256,"wd":274,"ws":40,"track":270.0,"baro_rate":-608,"geom_rate":-640,"squawk":"7511","emergency":"none",
 "category":"A5","lat":51.465454,"lon":-0.177612,"messages":230798,"seen":0.0,"rssi":-7.6,"dst":2.978,"dir":225.9},
{"hex":"781704","type":"adsb_icao","flight":"CSN303  ","r":"B-20C6","t":"B789","alt_baro":4550,"gs":136.0,"track":270.84,
 "baro_rate":-1248,"squawk":"3521","emergency":"none","category":"A5","lat":51.465317,"lon":-0.085293,"seen":1.5}
],"msg":"No error","now":1792952727000,"total":2})";

// opendata.adsb.fi/api/v2/... - identical fields, the array hangs off "aircraft" instead
const char* kAdsbFi = R"({"now":1792952728.001,"aircraft":[
{"hex":"40749b","flight":"BAW60   ","r":"G-ZBKR","t":"B789","alt_baro":3325,"gs":126.0,"track":270.0,"squawk":"7511",
 "emergency":"none","lat":51.465454,"lon":-0.177612,"seen":0.0}
],"resultCount":1})";

// opensky-network.org/api/states/all?... - positional arrays in a documented order, metres and metres per second
const char* kOpenSky = R"({"time":1792952719,"states":[
["4acb58","SAS811  ","Sweden",1792952719,1792952709,-0.2522,51.4654,731.52,false,67.91,270,-3.9,null,754.38,"2004",false,0],
["3c6444","DLH8LA  ","Germany",1792952718,1792952718,-0.1,51.5,null,true,0.0,180,null,null,null,null,false,0]
]})";

size_t findIcao(const std::vector<ApiAircraft>& v, uint32_t icao) {
    for (size_t i = 0; i < v.size(); i++) if (v[i].icao == icao) return i;
    return (size_t)-1;
}

}  // namespace

int main() {
    // ---- the service table is coherent: ids unique, a reachable service has a template, an unreachable one says why
    {
        const std::vector<ApiService>& all = apiServices();
        CHECK(all.size() >= 4, "only %zu services", all.size());
        for (size_t i = 0; i < all.size(); i++) {
            const ApiService& s = all[i];
            CHECK(s.id && *s.id, "service %zu has no id", i);
            CHECK(s.name && *s.name, "service %s has no name", s.id);
            CHECK(apiServiceById(s.id) == &all[i], "%s does not look up to itself", s.id);
            CHECK(s.minIntervalSec >= 1, "%s would poll every %.1f s", s.id, s.minIntervalSec);
            CHECK(s.maxRadiusNm > 0 && s.maxRadiusNm <= 1000, "%s has a %.0f nm maximum", s.id, s.maxRadiusNm);
            if (s.keyMode == ApiKeyMode::NeedsHeader)
                CHECK(s.note && *s.note, "%s cannot be reached and does not say why", s.id);
            else
                CHECK(s.urlTemplate && *s.urlTemplate, "%s is reachable but has no URL", s.id);
            // Anything the user must be let into, or pay for, must not claim open access.
            if (s.keyMode == ApiKeyMode::NeedsHeader) CHECK(!s.openAccess, "%s needs a header but claims open access", s.id);
        }
        for (size_t i = 0; i < all.size(); i++)
            for (size_t j = i + 1; j < all.size(); j++) CHECK(std::string(all[i].id) != all[j].id, "duplicate id %s", all[i].id);
        CHECK(apiServiceById("nope") == nullptr, "an unknown id resolved");
        CHECK(apiServiceById("") == nullptr, "an empty id resolved");
    }

    // ---- a key is the user's text and ends up in a command line: only URL-safe characters get through
    {
        CHECK(apiKeyLooksSafe("abc123"), "a plain key was refused");
        CHECK(apiKeyLooksSafe("user:password"), "user:password was refused");
        CHECK(apiKeyLooksSafe("a-b_c.d~e"), "the unreserved characters were refused");
        CHECK(apiKeyLooksSafe(""), "an empty key was refused");
        CHECK(!apiKeyLooksSafe("a b"), "a space was allowed");
        CHECK(!apiKeyLooksSafe("a'b"), "a single quote was allowed");
        CHECK(!apiKeyLooksSafe("a\"b"), "a double quote was allowed");
        CHECK(!apiKeyLooksSafe("a;rm -rf /"), "a semicolon was allowed");
        CHECK(!apiKeyLooksSafe("a$(id)"), "a command substitution was allowed");
        CHECK(!apiKeyLooksSafe("a`id`"), "a backtick was allowed");
        CHECK(!apiKeyLooksSafe("a&b"), "an ampersand was allowed");
        CHECK(!apiKeyLooksSafe("a\nb"), "a newline was allowed");
        CHECK(!apiKeyLooksSafe(std::string(300, 'a')), "an absurdly long key was allowed");
    }

    // ---- the URL builder
    {
        const ApiService* lol = apiServiceById("adsblol");
        CHECK(lol != nullptr, "adsb.lol is missing");
        std::string err;
        const std::string u = apiUrl(*lol, 51.5, -0.12, 25, "", &err);
        CHECK(u == "https://api.adsb.lol/v2/point/51.50000/-0.12000/25", "built '%s'", u.c_str());
        CHECK(err.empty(), "an error was reported: %s", err.c_str());
        CHECK(u.find('{') == std::string::npos, "a placeholder was left in '%s'", u.c_str());

        // the radius is clamped to what the service allows, so a free network is never asked for the continent
        const std::string big = apiUrl(*lol, 51.5, -0.12, 99999, "", &err);
        CHECK(big.find("/250") != std::string::npos, "the radius was not clamped: %s", big.c_str());
        const std::string tiny = apiUrl(*lol, 51.5, -0.12, 0, "", &err);
        CHECK(tiny.find("/1") != std::string::npos, "a zero radius produced %s", tiny.c_str());

        // a bad position is refused rather than sent
        CHECK(apiUrl(*lol, 999, 0, 25, "", &err).empty() && !err.empty(), "a nonsense latitude was accepted");
        CHECK(apiUrl(*lol, 0, 999, 25, "", &err).empty(), "a nonsense longitude was accepted");

        // an unsafe key never reaches the address
        CHECK(apiUrl(*lol, 51.5, -0.12, 25, "a'; rm -rf /", &err).empty() && !err.empty(), "an unsafe key was accepted");

        // a service this program cannot reach says so instead of building something broken
        const ApiService* fr = apiServiceById("fr24");
        CHECK(fr != nullptr, "the Flightradar24 entry is missing");
        CHECK(apiUrl(*fr, 51.5, -0.12, 25, "token", &err).empty() && !err.empty(), "a header-only service produced a URL");
    }

    // ---- OpenSky wants a box, and the key goes in front of the host
    {
        const ApiService* os = apiServiceById("opensky");
        CHECK(os != nullptr, "OpenSky is missing");
        std::string err;
        const std::string anon = apiUrl(*os, 51.5, -0.12, 60, "", &err);
        CHECK(anon.find("https://opensky-network.org/") == 0, "without a key it built '%s'", anon.c_str());
        CHECK(anon.find("lamin=50.5") != std::string::npos, "the box latitude is wrong: %s", anon.c_str());
        const std::string named = apiUrl(*os, 51.5, -0.12, 60, "me:secret", &err);
        CHECK(named.find("https://me:secret@opensky-network.org/") == 0, "with a key it built '%s'", named.c_str());
    }

    // ---- the box around a circle
    {
        double a, b, c, d;
        apiBoundingBox(51.5, -0.12, 60, a, b, c, d);
        CHECK(std::fabs(a - 50.5) < 0.01 && std::fabs(c - 52.5) < 0.01, "latitudes %.3f .. %.3f", a, c);
        CHECK(b < -0.12 && d > -0.12, "longitudes %.3f .. %.3f do not contain the centre", b, d);
        // further north the same circle spans more longitude
        double a2, b2, c2, d2;
        apiBoundingBox(70, 0, 60, a2, b2, c2, d2);
        CHECK((d2 - b2) > (d - b), "the box did not widen towards the pole (%.2f vs %.2f)", d2 - b2, d - b);
        // at the pole, and across the date line, it asks for the whole band rather than an inside-out box
        apiBoundingBox(90, 0, 60, a2, b2, c2, d2);
        CHECK(b2 <= -180.0 && d2 >= 180.0, "at the pole the box is %.1f .. %.1f", b2, d2);
        apiBoundingBox(0, 179.9, 120, a2, b2, c2, d2);
        CHECK(b2 <= -180.0 && d2 >= 180.0, "across the date line the box is %.1f .. %.1f", b2, d2);
        apiBoundingBox(0, 0, 60, a2, b2, c2, d2);
        CHECK(a2 < c2 && b2 < d2, "the box is inside out");
    }

    // ---- a readsb-style answer, from the real thing
    {
        std::vector<ApiAircraft> v;
        std::string err;
        size_t skipped = 99;
        CHECK(parseApiResponse(ApiShape::ReadsbJson, kAdsbLol, "ac", v, &err, &skipped), "adsb.lol failed: %s", err.c_str());
        CHECK(v.size() == 2, "read %zu aircraft", v.size());
        CHECK(skipped == 0, "%zu aircraft were skipped", skipped);
        const size_t i = findIcao(v, 0x40749b);
        CHECK(i != (size_t)-1, "40749B is missing");
        if (i != (size_t)-1) {
            const ApiAircraft& a = v[i];
            CHECK(a.callsign == "BAW60", "the call sign is '%s' - the padding was not trimmed", a.callsign.c_str());
            CHECK(a.registration == "G-ZBKR", "registration '%s'", a.registration.c_str());
            CHECK(a.type == "B789", "type '%s'", a.type.c_str());
            CHECK(a.category == "A5", "category '%s'", a.category.c_str());
            CHECK(a.hasPos && std::fabs(a.lat - 51.465454) < 1e-5 && std::fabs(a.lon + 0.177612) < 1e-5, "position %.5f %.5f", a.lat, a.lon);
            CHECK(a.hasAlt && a.altFt == 3325, "altitude %d", a.altFt);
            CHECK(a.hasSpeed && std::fabs(a.speedKt - 126) < 0.5, "speed %.1f", a.speedKt);
            CHECK(a.hasHeading && std::fabs(a.headingDeg - 270) < 0.5, "track %.1f", a.headingDeg);
            CHECK(a.hasVrate && a.vrateFpm == -608, "vertical rate %d", a.vrateFpm);
            // the squawk arrives as the text "7511" and must read as those four octal digits, not as hex or a code point
            CHECK(a.hasSquawk && a.squawk == 7511, "squawk %d", a.squawk);
            CHECK(!a.emergency, "\"none\" was read as an emergency");
            CHECK(!a.ground, "it was called on the ground");
        }
    }

    // ---- the same fields behind a different array key
    {
        std::vector<ApiAircraft> v;
        std::string err;
        CHECK(parseApiResponse(ApiShape::ReadsbJson, kAdsbFi, "aircraft", v, &err), "adsb.fi failed: %s", err.c_str());
        CHECK(v.size() == 1 && v[0].icao == 0x40749b, "read %zu aircraft", v.size());
        // and the wrong key still finds it, so one mistaken preset does not look like an empty sky
        std::vector<ApiAircraft> w;
        CHECK(parseApiResponse(ApiShape::ReadsbJson, kAdsbFi, "ac", w, &err), "the fallback key failed: %s", err.c_str());
        CHECK(w.size() == 1, "the fallback read %zu aircraft", w.size());
    }

    // ---- OpenSky: positional, and in metres
    {
        std::vector<ApiAircraft> v;
        std::string err;
        CHECK(parseApiResponse(ApiShape::OpenSkyStates, kOpenSky, "states", v, &err), "OpenSky failed: %s", err.c_str());
        CHECK(v.size() == 2, "read %zu states", v.size());
        const size_t i = findIcao(v, 0x4acb58);
        CHECK(i != (size_t)-1, "4ACB58 is missing");
        if (i != (size_t)-1) {
            const ApiAircraft& a = v[i];
            CHECK(a.callsign == "SAS811", "call sign '%s'", a.callsign.c_str());
            CHECK(a.hasPos && std::fabs(a.lat - 51.4654) < 1e-4 && std::fabs(a.lon + 0.2522) < 1e-4, "position %.4f %.4f", a.lat, a.lon);
            // 731.52 m is 2400 ft, 67.91 m/s is 132 kt, -3.9 m/s is -768 ft/min
            CHECK(a.hasAlt && std::abs(a.altFt - 2400) <= 2, "altitude %d ft, expected about 2400", a.altFt);
            CHECK(a.hasSpeed && std::fabs(a.speedKt - 132) < 1.0, "speed %.1f kt, expected about 132", a.speedKt);
            CHECK(a.hasVrate && std::abs(a.vrateFpm + 768) <= 3, "vertical rate %d fpm, expected about -768", a.vrateFpm);
            CHECK(a.hasSquawk && a.squawk == 2004, "squawk %d", a.squawk);
            CHECK(!a.ground, "it was called on the ground");
            CHECK(a.seenSec > 9 && a.seenSec < 11, "last contact read as %.1f s ago", a.seenSec);
        }
        const size_t j = findIcao(v, 0x3c6444);
        if (j != (size_t)-1) {
            CHECK(v[j].ground, "the on-ground aircraft was not marked");
            CHECK(!v[j].hasAlt, "a null altitude became %d ft", v[j].altFt);
            CHECK(!v[j].hasSquawk, "a null squawk became %d", v[j].squawk);
        }
    }

    // ---- an aircraft on the ground, where alt_baro is the word rather than a number
    {
        std::vector<ApiAircraft> v;
        std::string err;
        const char* body = R"({"ac":[{"hex":"abcdef","flight":"TEST1","alt_baro":"ground","gs":12.0,"lat":51.0,"lon":0.0}]})";
        CHECK(parseApiResponse(ApiShape::ReadsbJson, body, "ac", v, &err), "failed: %s", err.c_str());
        CHECK(v.size() == 1 && v[0].ground, "\"ground\" was not understood");
        CHECK(!v[0].hasAlt, "\"ground\" became an altitude of %d ft", v[0].altFt);
    }

    // ---- a number sent as a string, which these services do
    {
        std::vector<ApiAircraft> v;
        std::string err;
        const char* body = R"({"ac":[{"hex":"abcdef","alt_baro":"3500","gs":"210.5","track":"90","lat":"51.0","lon":"0.5"}]})";
        CHECK(parseApiResponse(ApiShape::ReadsbJson, body, "ac", v, &err), "failed: %s", err.c_str());
        CHECK(v.size() == 1, "read %zu", v.size());
        if (v.size() == 1) {
            CHECK(v[0].hasAlt && v[0].altFt == 3500, "altitude %d", v[0].altFt);
            CHECK(v[0].hasSpeed && std::fabs(v[0].speedKt - 210.5) < 0.01, "speed %.2f", v[0].speedKt);
            CHECK(v[0].hasPos, "a position sent as text was not read");
        }
    }

    // ---- one bad row does not cost the others
    {
        std::vector<ApiAircraft> v;
        std::string err;
        size_t skipped = 0;
        const char* body = R"({"ac":[{"hex":"zzzzzz"},{"nohex":1},{"hex":"40749b","lat":51.0,"lon":0.0},"not an object",{"hex":""}]})";
        CHECK(parseApiResponse(ApiShape::ReadsbJson, body, "ac", v, &err, &skipped), "failed: %s", err.c_str());
        CHECK(v.size() == 1 && v[0].icao == 0x40749b, "read %zu aircraft, expected the one good row", v.size());
        CHECK(skipped == 4, "%zu rows were skipped, expected 4", skipped);
    }

    // ---- answers that are not aircraft at all
    {
        std::vector<ApiAircraft> v;
        std::string err;
        CHECK(!parseApiResponse(ApiShape::ReadsbJson, "", "ac", v, &err), "an empty body was accepted");
        CHECK(!parseApiResponse(ApiShape::ReadsbJson, "not json", "ac", v, &err), "rubbish was accepted");
        CHECK(!parseApiResponse(ApiShape::ReadsbJson, "[1,2,3]", "ac", v, &err), "a bare array was accepted");
        CHECK(!parseApiResponse(ApiShape::ReadsbJson, R"({"total":0})", "ac", v, &err), "an answer with no list was accepted");
        // the real refusal airplanes.live sends, which must read as a refusal rather than as an empty sky
        const char* refused = R"({"error": "Please contact us at contact@airplanes.live. Your email MUST include any links, a description of the project, and any information you deem appropriate."})";
        CHECK(!parseApiResponse(ApiShape::ReadsbJson, refused, "ac", v, &err), "a refusal was read as an answer");
        CHECK(err.find("refused") != std::string::npos, "the refusal was reported as '%s'", err.c_str());
        // an empty list is a real answer: no aircraft there, no error
        CHECK(parseApiResponse(ApiShape::ReadsbJson, R"({"ac":[]})", "ac", v, &err), "an empty list failed: %s", err.c_str());
        CHECK(v.empty(), "an empty list produced %zu aircraft", v.size());
        // OpenSky sends null states for an empty box
        CHECK(parseApiResponse(ApiShape::OpenSkyStates, R"({"time":1,"states":null})", "states", v, &err), "null states failed: %s", err.c_str());
        CHECK(v.empty(), "null states produced %zu aircraft", v.size());
        // a short state vector is not one
        CHECK(parseApiResponse(ApiShape::OpenSkyStates, R"({"time":1,"states":[["abc","x"]]})", "states", v, &err), "failed: %s", err.c_str());
        CHECK(v.empty(), "a truncated state vector was read");
    }

    // ---- every aircraft alike: nothing is dropped, or treated differently, for the database flags a service attaches
    //
    // readsb-based services mark aircraft in "dbFlags": 1 military, 2 interesting, 4 PIA, 8 LADD. This program takes no part in LADD and
    // filters nothing by any of them, so an aircraft carrying any of those flags must come through exactly as one carrying none.
    {
        const char* body = R"({"ac":[
            {"hex":"a00001","flight":"PLAIN1","alt_baro":30000,"lat":40.0,"lon":-75.0},
            {"hex":"a00002","flight":"LADD1","alt_baro":30000,"lat":40.0,"lon":-75.0,"dbFlags":8},
            {"hex":"a00003","flight":"PIA1","alt_baro":30000,"lat":40.0,"lon":-75.0,"dbFlags":4},
            {"hex":"a00004","flight":"BOTH1","alt_baro":30000,"lat":40.0,"lon":-75.0,"dbFlags":12},
            {"hex":"a00005","flight":"MIL1","alt_baro":30000,"lat":40.0,"lon":-75.0,"dbFlags":1},
            {"hex":"a00006","flight":"ALL1","alt_baro":30000,"lat":40.0,"lon":-75.0,"dbFlags":15}
        ]})";
        std::vector<ApiAircraft> v;
        std::string err;
        size_t skipped = 0;
        CHECK(parseApiResponse(ApiShape::ReadsbJson, body, "ac", v, &err, &skipped), "failed: %s", err.c_str());
        CHECK(v.size() == 6, "%zu of 6 aircraft came through: a flagged aircraft was dropped", v.size());
        CHECK(skipped == 0, "%zu flagged aircraft were skipped", skipped);
        // and each one is read in full, the same as the unflagged one
        const size_t plain = findIcao(v, 0xa00001);
        for (uint32_t icao : {0xa00002u, 0xa00003u, 0xa00004u, 0xa00005u, 0xa00006u}) {
            const size_t i = findIcao(v, icao);
            CHECK(i != (size_t)-1, "%06X is missing", icao);
            if (i == (size_t)-1 || plain == (size_t)-1) continue;
            CHECK(v[i].hasPos && v[i].hasAlt && !v[i].callsign.empty(), "%06X was not read in full", icao);
            CHECK(v[i].lat == v[plain].lat && v[i].altFt == v[plain].altFt, "%06X was treated differently from the unflagged one", icao);
            const AdsbExtUpdate u = apiToExtUpdate(v[i]);
            CHECK(u.icao == icao && u.hasPos && u.hasAlt, "%06X did not reach the table whole", icao);
        }
    }

    // ---- what reaches the aircraft table
    {
        ApiAircraft a;
        a.icao = 0x40749b;
        a.callsign = "BAW60";
        a.hasPos = true; a.lat = 51.4; a.lon = -0.1;
        a.hasAlt = true; a.altFt = 3325;
        a.hasSpeed = true; a.speedKt = 126;
        a.hasHeading = true; a.headingDeg = 270;
        a.hasVrate = true; a.vrateFpm = -608;
        a.hasSquawk = true; a.squawk = 7511;
        const AdsbExtUpdate u = apiToExtUpdate(a);
        CHECK(u.icao == 0x40749b && u.callsign == "BAW60", "identity did not carry over");
        CHECK(u.hasPos && u.lat == 51.4 && u.lon == -0.1, "the position did not carry over");
        CHECK(u.hasAlt && u.altFt == 3325 && u.hasSpeed && u.hasHeading && u.hasVrate, "a measurement did not carry over");
        CHECK(u.hasSquawk && u.squawk == 7511, "the squawk did not carry over");
        CHECK(u.hasGround, "the ground flag is not set either way");

        ApiAircraft bare;              // nothing known but the address
        bare.icao = 0xABCDEF;
        const AdsbExtUpdate b = apiToExtUpdate(bare);
        CHECK(b.icao == 0xABCDEF, "the address did not carry over");
        CHECK(!b.hasPos && !b.hasAlt && !b.hasSpeed && !b.hasSquawk, "a field was invented out of nothing");
    }

    return testSummary("adsb_api");
}
