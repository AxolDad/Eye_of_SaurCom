// Aircraft from an aggregator's web API: the services, their URLs, and the readers for what they send back.
//
// This is the other direction from feed_targets.h. There you send what your antenna heard to a network; here you ask a network what is in the
// air somewhere, including where your own antenna cannot reach. Aircraft that arrive this way are SHOWN, never kept and never passed on:
//
//   - they go into the aircraft table as a source of their own (AdsbTelemetry::sources, srcId > 0), so the map and the table mark where each
//     one came from;
//   - AdsbTracker::applyExternal never touches the outbound feeds, so a position you did not receive is never re-sent to an aggregator - that
//     would pollute their network and break their terms;
//   - the flight recorder (flights/flights_rec.h) skips every aircraft with srcId != 0, because the flight history answers "what flew over
//     this antenna" and an API covering a whole region would quietly make those answers wrong.
//
// Everything here is pure: the service table, the URL builder and the readers. The polling and the HTTP live in the application layer, which
// is where this program's fetch helper lives (as the Deep Space Network feed is arranged too).
//
// Nothing is built in but the addresses. No key, account or token is shipped; a service that needs one gets it from the user.
#pragma once
#include "dect2/adsb/adsb_track.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// The two shapes these services answer in. They are worth exactly two readers, not one per service: every readsb-derived aggregator sends the
// same field names (hex, flight, r, t, alt_baro, gs, track, squawk ...) and differs only in the key its array hangs off.
enum class ApiShape {
    ReadsbJson,     // {"ac":[{...}]} or {"aircraft":[{...}]} - adsb.lol, adsb.fi, airplanes.live, ADSB Exchange
    OpenSkyStates,  // {"time":..,"states":[[icao,callsign,country,...]]} - positional arrays, documented order
};

// How a key reaches a service. This program's fetch helper sends no request headers, so a service whose key must go in an Authorization
// header cannot be reached yet; those are listed with NeedsHeader so the window can say so rather than fail silently.
enum class ApiKeyMode { None, Query, UserInfo, NeedsHeader };

struct ApiService {
    const char* id;              // stable across versions: the preferences key
    const char* name;
    ApiShape shape = ApiShape::ReadsbJson;
    // {lat} {lon} {radiusNm} are the circle asked for; {latMin} {lonMin} {latMax} {lonMax} the box around it; {key} the user's key.
    const char* urlTemplate = "";
    const char* arrayKey = "ac";   // ReadsbJson: the key the aircraft array hangs off
    ApiKeyMode keyMode = ApiKeyMode::None;
    double minIntervalSec = 5;   // the service's own rate limit: never poll faster than this
    double maxRadiusNm = 250;
    bool openAccess = true;      // false: the service decides who may use it, or wants paying. Shown, and off until it works for the user.
    const char* note = "";
    const char* docsUrl = "";
};

// Every service this program knows how to ask. Off until the user enables one.
const std::vector<ApiService>& apiServices();
const ApiService* apiServiceById(const std::string& id);

// One aircraft as a service described it. Only a field whose has... flag is set means anything.
struct ApiAircraft {
    uint32_t icao = 0;
    std::string callsign;        // trimmed: these services pad it with spaces
    std::string registration;    // "r" where the service knows it
    std::string type;            // "t": B738, A320
    std::string category;        // "A3"
    bool hasPos = false; double lat = 0, lon = 0;
    bool hasAlt = false; int altFt = 0;
    bool ground = false;
    bool hasSpeed = false; double speedKt = 0;
    bool hasHeading = false; double headingDeg = 0;
    bool hasVrate = false; int vrateFpm = 0;
    bool hasSquawk = false; int squawk = 0;
    bool emergency = false;
    double seenSec = 0;          // how long ago the service last heard it, where it says
};

// Reads a response. False with *err when it cannot be read at all; a single unreadable aircraft is skipped and counted in *skipped rather
// than failing the lot, because one odd row should not cost the other two hundred.
bool parseApiResponse(ApiShape shape, const std::string& body, const std::string& arrayKey, std::vector<ApiAircraft>& out,
                      std::string* err = nullptr, size_t* skipped = nullptr);

// The URL to ask a service for everything within radiusNm of a position. "" with *err when the service needs a key and has none, when the key
// has characters that must not go in a URL, or when the service cannot be reached by this program at all.
// The radius is clamped to the service's own maximum: asking a free service for the whole continent is neither polite nor useful.
std::string apiUrl(const ApiService& s, double latDeg, double lonDeg, double radiusNm, const std::string& key, std::string* err = nullptr);

// A key is the user's own text and ends up inside a command line, so only the characters a URL token is allowed to hold are accepted:
// letters, digits and - _ . ~ : and nothing else. False for anything else, including spaces and quotes.
bool apiKeyLooksSafe(const std::string& key);

// What the aircraft table takes. The position is used as the service gave it: there is no MLAT and nothing is recomputed here.
AdsbExtUpdate apiToExtUpdate(const ApiAircraft& a);

// The box around a circle, for the services that ask for one. Clamped to the poles and wrapped at the date line.
void apiBoundingBox(double latDeg, double lonDeg, double radiusNm, double& latMin, double& lonMin, double& latMax, double& lonMax);

}  // namespace dect2
