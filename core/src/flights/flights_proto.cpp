// Flights out of the live aircraft table: see flights_proto.h
#include "dect2/flights/flights_proto.h"
#include "dect2/adsb/adsb_tel.h"
#include "dect2/airports/airports.h"
#include "dect2/track/track_geo.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace dect2::flights {

using track::gcDistM;
using track::kNm;
using track::wrap180;
using track::wrap360;

namespace {

double distNm(double lat1, double lon1, double lat2, double lon2) { return gcDistM(lat1, lon1, lat2, lon2) / kNm; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

std::string upper(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::toupper((unsigned char)c);
    return o;
}

// true bearing from one position to another
double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * track::kDeg, p2 = lat2 * track::kDeg, dl = wrap180(lon2 - lon1) * track::kDeg;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    return wrap360(std::atan2(y, x) / track::kDeg);
}

// Distance from a point to a line segment, all in a local east/north frame, metres.
double pointSegM(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    if (len2 <= 0) return std::hypot(px - ax, py - ay);
    double t = ((px - ax) * dx + (py - ay) * dy) / len2;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

} // namespace

const char* phaseName(Phase p) {
    switch (p) {
    case PhGround: return "ground";
    case PhTakeoff: return "takeoff";
    case PhClimb: return "climb";
    case PhCruise: return "cruise";
    case PhDescent: return "descent";
    case PhApproach: return "approach";
    case PhLanding: return "landing";
    default: return "";
    }
}

const char* endKindName(EndKind k) {
    switch (k) {
    case EndKind::Airport: return "on the ground at";
    case EndKind::NearAirport: return "low beside";
    case EndKind::Overhead: return "headed for";
    default: return "out of range";
    }
}

const char* operationKindName(OperationKind k) {
    switch (k) {
    case OpAirline: return "airline";
    case OpCargo: return "cargo";
    case OpPrivate: return "private";
    case OpMedical: return "medical";
    case OpMilitary: return "military";
    case OpSurvey: return "survey";
    case OpSkydive: return "skydive";
    case OpTraining: return "training";
    default: return "unknown";
    }
}

// ---------------------------------------------------------------- phases

void classifyPhases(std::vector<Point>& pts, const PhaseLimits& lim, int fieldElevFt) {
    const size_t n = pts.size();
    if (!n) return;
    // The floor to measure "near the ground" against. Without an airport elevation the lowest point of the track has to serve, which is
    // only meaningful if the flight actually changed height: an aircraft that crossed the whole span level at 37000 ft is not on the
    // ground at 37000 ft, so for a track with no vertical extent nothing counts as near the ground unless the aircraft said so itself.
    int floorFt = fieldElevFt;
    bool floorIsGuess = false;
    if (floorFt == -1000000) {
        int lo = INT_MAX, hi = INT_MIN;
        for (const auto& p : pts) if (p.hasAlt) { lo = std::min(lo, p.altFt); hi = std::max(hi, p.altFt); }
        if (lo == INT_MAX) { floorFt = 0; floorIsGuess = true; }
        else { floorFt = lo; floorIsGuess = hi - lo <= lim.groundAltFt * 2; }
    }
    // a vertical rate per point: what was reported, else differentiated from the altitudes over a few seconds
    std::vector<double> vr(n, 0.0);
    std::vector<bool> hasVr(n, false);
    for (size_t i = 0; i < n; i++) {
        if (pts[i].hasVrate) { vr[i] = pts[i].vrateFpm; hasVr[i] = true; continue; }
        if (!pts[i].hasAlt) continue;
        // the nearest points at least 8 s away on each side
        size_t a = i, b = i;
        while (a > 0 && pts[i].t - pts[a].t < 8.0) a--;
        while (b + 1 < n && pts[b].t - pts[i].t < 8.0) b++;
        if (a != b && pts[a].hasAlt && pts[b].hasAlt && pts[b].t > pts[a].t) {
            vr[i] = (pts[b].altFt - pts[a].altFt) / ((pts[b].t - pts[a].t) / 60.0);
            hasVr[i] = true;
        }
    }
    // smooth over three points so one bad altitude does not change the phase
    std::vector<double> vs = vr;
    for (size_t i = 0; i < n; i++) {
        double sum = 0; int cnt = 0;
        for (size_t k = (i ? i - 1 : 0); k <= i + 1 && k < n; k++) if (hasVr[k]) { sum += vr[k]; cnt++; }
        vs[i] = cnt ? sum / cnt : 0;
    }
    for (size_t i = 0; i < n; i++) {
        Point& p = pts[i];
        const bool nearGround = !floorIsGuess && p.hasAlt && p.altFt - floorFt <= lim.groundAltFt;
        const bool slow = p.hasSpeed && p.speedKt < lim.groundSpeedKt;
        if (p.ground || (nearGround && slow)) { p.phase = PhGround; continue; }
        const double v = vs[i];
        if (v > lim.climbFpm) {
            p.phase = nearGround ? PhTakeoff : PhClimb;
        } else if (v < -lim.climbFpm) {
            p.phase = nearGround ? PhLanding : PhDescent;
        } else {
            p.phase = nearGround ? PhUnknown : PhCruise;   // level near the ground: resolved below
        }
    }
    // Level and near the ground: a take-off roll if it climbs away soon after, a landing roll if it was coming down just before,
    // otherwise it is simply standing or taxiing.
    for (size_t i = 0; i < n; i++) {
        if (pts[i].phase != PhUnknown) continue;
        bool climbsAfter = false, descendedBefore = false;
        for (size_t k = i + 1; k < n && pts[k].t - pts[i].t <= 60; k++)
            if (vs[k] > lim.climbFpm) { climbsAfter = true; break; }
        for (size_t k = i; k-- > 0 && pts[i].t - pts[k].t <= 60;)
            if (vs[k] < -lim.climbFpm) { descendedBefore = true; break; }
        pts[i].phase = climbsAfter ? PhTakeoff : descendedBefore ? PhLanding : PhGround;
    }
    // an aircraft that came to the ground: the points just before it are the landing, not a descent
    for (size_t i = 1; i < n; i++)
        if ((pts[i].phase == PhGround || pts[i].phase == PhLanding) && pts[i - 1].phase == PhDescent) {
            for (size_t k = i; k-- > 0;) {
                if (pts[k].phase != PhDescent || pts[i].t - pts[k].t > 60) break;
                pts[k].phase = PhLanding;
            }
        }
    // The climb straight off the runway is still the take-off, but only while the aircraft is close to the field: a run forward from a
    // roll, ending once it is takeoffAltFt above the floor or a minute has passed. (Marking it by looking at the point before would
    // cascade through the whole climb, since that point has just been relabelled.)
    for (size_t i = 1; i < n; i++) {
        if (pts[i - 1].phase != PhTakeoff && pts[i - 1].phase != PhGround) continue;
        const double t0 = pts[i - 1].t;
        for (size_t k = i; k < n; k++) {
            if (pts[k].phase != PhClimb) break;
            if (pts[k].t - t0 > 60) break;
            if (pts[k].hasAlt && pts[k].altFt - floorFt > lim.takeoffAltFt) break;
            pts[k].phase = PhTakeoff;
        }
    }
}

void markApproachPhase(std::vector<Point>& pts, const AirportDb& db, const PhaseLimits& lim) {
    // The descent into an airport is its own phase: a point coming down within approachRangeNm of a known airport and below
    // approachAltFt above its elevation. Only the run of points leading to the end of the flight counts, so a descent over an
    // airport on the way past does not become an approach.
    if (pts.empty()) return;
    const Point& last = pts.back();
    const Airport* a = db.nearestWithRunway(last.lat, last.lon, lim.approachRangeNm * kNm / 1000.0, 0);
    if (!a) return;
    for (size_t i = pts.size(); i-- > 0;) {
        Point& p = pts[i];
        if (p.phase == PhGround || p.phase == PhLanding) continue;   // the roll at the end
        if (p.phase != PhDescent && p.phase != PhCruise) break;      // back before the descent began
        if (!p.hasAlt || p.altFt - a->elevFt > lim.approachAltFt) break;
        if (distNm(p.lat, p.lon, a->latDeg, a->lonDeg) > lim.approachRangeNm) break;
        if (p.phase == PhDescent) p.phase = PhApproach;
    }
}

// ---------------------------------------------------------------- the figures

void summarise(Flight& f, const std::vector<Point>& pts, const double* rxLatLon) {
    f.points = pts.size();
    if (pts.empty()) return;
    f.tStart = pts.front().t;
    f.tEnd = pts.back().t;
    f.distanceNm = 0;
    f.airborneSec = 0;
    for (auto& s : f.phaseSec) s = 0;
    f.maxAltFt = INT_MIN; f.maxSpeedKt = 0; f.maxVrateFpm = 0; f.minVrateFpm = 0;
    f.hasAlt = false; f.hasSpeed = false;
    f.minRangeNm = 0; f.maxRangeNm = 0; f.hasRange = false;
    std::map<int, double> levelSec;      // altitude band (500 ft) -> seconds spent level there
    std::map<int, double> cruiseSpd;     // speed band (10 kt) -> seconds while level
    bool havePrevPos = false;
    double prevLat = 0, prevLon = 0;
    for (size_t i = 0; i < pts.size(); i++) {
        const Point& p = pts[i];
        const double dt = i + 1 < pts.size() ? pts[i + 1].t - p.t : (pts.size() > 1 ? p.t - pts[i - 1].t : 0);
        const double span = dt > 0 && dt < 600 ? dt : 0;   // a long gap is not time spent in this phase
        if (p.phase < 8) f.phaseSec[p.phase] += (uint32_t)(span + 0.5);
        if (!p.ground && p.phase != PhGround) f.airborneSec += span;
        if (p.hasAlt) {
            f.hasAlt = true;
            if (p.altFt > f.maxAltFt) f.maxAltFt = p.altFt;
            if (p.phase == PhCruise) levelSec[(p.altFt / 500) * 500] += span > 0 ? span : 1;
        }
        if (p.hasSpeed) {
            f.hasSpeed = true;
            if (p.speedKt > f.maxSpeedKt) f.maxSpeedKt = p.speedKt;
            if (p.phase == PhCruise) cruiseSpd[((int)p.speedKt / 10) * 10] += span > 0 ? span : 1;
        }
        if (p.hasVrate) {
            if (p.vrateFpm > f.maxVrateFpm) f.maxVrateFpm = p.vrateFpm;
            if (p.vrateFpm < f.minVrateFpm) f.minVrateFpm = p.vrateFpm;
        }
        if (p.ground) f.sawGround = true;
        if (p.phase == PhTakeoff) f.sawTakeoff = true;
        if (p.phase == PhLanding) f.sawLanding = true;
        if (havePrevPos) f.distanceNm += (float)distNm(prevLat, prevLon, p.lat, p.lon);
        prevLat = p.lat; prevLon = p.lon; havePrevPos = true;
        if (rxLatLon) {
            const float r = (float)distNm(rxLatLon[0], rxLatLon[1], p.lat, p.lon);
            if (!f.hasRange) { f.minRangeNm = f.maxRangeNm = r; f.hasRange = true; }
            else { f.minRangeNm = std::min(f.minRangeNm, r); f.maxRangeNm = std::max(f.maxRangeNm, r); }
        }
    }
    if (!f.hasAlt) f.maxAltFt = 0;
    // the altitude and speed it spent longest at while level
    f.cruiseAltFt = 0; f.cruiseSpeedKt = 0;
    double best = 0;
    for (const auto& kv : levelSec) if (kv.second > best) { best = kv.second; f.cruiseAltFt = kv.first; }
    best = 0;
    for (const auto& kv : cruiseSpd) if (kv.second > best) { best = kv.second; f.cruiseSpeedKt = (float)kv.first; }
    f.routeSignature = routeSignature(pts);
}

// ---------------------------------------------------------------- where it began and ended

namespace {

// The airport nearest a point that has a runway at least minRunwayFt long, with the distance.
const Airport* nearAirport(const AirportDb& db, double lat, double lon, float radiusNm, int minRunwayFt, float& outNm) {
    const Airport* a = db.nearestWithRunway(lat, lon, radiusNm * kNm / 1000.0, minRunwayFt);
    if (!a) { outNm = 0; return nullptr; }
    outNm = (float)distNm(lat, lon, a->latDeg, a->lonDeg);
    return a;
}

// The runway end of an airport that best matches a track, when the aircraft was flying towards one of them.
const RunwayEnd* runwayForTrack(const Airport& a, double trackDeg, std::string& outIdent) {
    const RunwayEnd* best = nullptr;
    double bestDiff = 40.0;   // the track has to be roughly down the runway
    for (const auto& r : a.runways) {
        for (const RunwayEnd* e : {&r.le, &r.he}) {
            if (!e->hasHeading || !e->hasPos) continue;
            const double d = std::fabs(wrap180(trackDeg - e->headingDegT));
            if (d < bestDiff) { bestDiff = d; best = e; }
        }
    }
    if (best) outIdent = best->ident;
    return best;
}

void fillEnd(FlightEnd& end, const Point& p, const AirportDb& db, float radiusNm, int minRunwayFt, bool onGround, double trackDeg, bool hasTrack) {
    end.t = p.t; end.lat = p.lat; end.lon = p.lon; end.altFt = p.hasAlt ? p.altFt : 0;
    float nm = 0;
    const Airport* a = nearAirport(db, p.lat, p.lon, radiusNm, minRunwayFt, nm);
    if (!a) { end.kind = EndKind::None; return; }
    end.icao = a->icao;
    end.distNm = nm;
    end.kind = onGround ? EndKind::Airport : (p.hasAlt && p.altFt - a->elevFt < 1500 && nm < radiusNm * 0.5f) ? EndKind::NearAirport : EndKind::Overhead;
    if (hasTrack) runwayForTrack(*a, trackDeg, end.runway);
}

} // namespace

void findEnds(Flight& f, const std::vector<Point>& pts, const AirportDb& db, float radiusNm, int minRunwayFt) {
    f.origin = FlightEnd();
    f.destination = FlightEnd();
    if (pts.empty()) return;
    const Point& first = pts.front();
    const Point& last = pts.back();
    // the track out of the first points and into the last, so a runway can be matched
    double outTrack = 0, inTrack = 0;
    bool haveOut = false, haveIn = false;
    for (size_t i = 1; i < pts.size() && i < 12; i++)
        if (distNm(first.lat, first.lon, pts[i].lat, pts[i].lon) > 0.3) { outTrack = bearingDeg(first.lat, first.lon, pts[i].lat, pts[i].lon); haveOut = true; break; }
    for (size_t i = pts.size(); i-- > 0 && pts.size() - i < 12;)
        if (distNm(pts[i].lat, pts[i].lon, last.lat, last.lon) > 0.3) { inTrack = bearingDeg(pts[i].lat, pts[i].lon, last.lat, last.lon); haveIn = true; break; }
    fillEnd(f.origin, first, db, radiusNm, minRunwayFt, first.ground || first.phase == PhGround, outTrack, haveOut);
    fillEnd(f.destination, last, db, radiusNm, minRunwayFt, last.ground || last.phase == PhGround, inTrack, haveIn);
    // an aircraft that only flew past: it did not depart from or arrive at anything
    if (f.origin.kind == EndKind::Overhead && !f.sawTakeoff) f.origin.kind = EndKind::None;
    if (f.destination.kind == EndKind::Overhead && !f.sawLanding && f.destination.distNm > radiusNm * 0.5f) f.destination.kind = EndKind::None;
}

// ---------------------------------------------------------------- the approach

bool fitApproach(const std::vector<Point>& pts, const AirportDb& db, ApproachFit& out, float glideDeg) {
    out = ApproachFit();
    out.glideDeg = glideDeg;
    if (pts.size() < 4) return false;
    // the last point low enough to be an arrival, and the airport it is at
    const Point& last = pts.back();
    float nm = 0;
    const Airport* a = nearAirport(db, last.lat, last.lon, out.fromNm + 5, 0, nm);
    if (!a || a->runways.empty()) return false;
    // which end: the track over the final mile
    double trk = 0;
    bool haveTrk = false;
    for (size_t i = pts.size(); i-- > 0;)
        if (distNm(pts[i].lat, pts[i].lon, last.lat, last.lon) > 0.5) { trk = bearingDeg(pts[i].lat, pts[i].lon, last.lat, last.lon); haveTrk = true; break; }
    if (!haveTrk && last.hasTrack) { trk = last.trackDeg; haveTrk = true; }
    if (!haveTrk) return false;
    std::string ident;
    const RunwayEnd* end = runwayForTrack(*a, trk, ident);
    if (!end) return false;

    // a local frame on the threshold, with the runway heading as the x axis
    track::Frame fr;
    fr.set(end->latDeg, end->lonDeg);
    const double hdg = end->headingDegT * track::kDeg;
    const double thrElev = end->hasElev ? end->elevFt : a->elevFt;
    double latSum = 0, vertSum = 0;
    int cnt = 0;
    float prevAlongNm = 0, prevHeight = 0;
    bool havePrev = false;
    for (const Point& p : pts) {
        double e = 0, n = 0;
        fr.toLocal(p.lat, p.lon, e, n);
        // along: positive out in front of the threshold (the direction an arriving aircraft comes from)
        const double along = -(e * std::sin(hdg) + n * std::cos(hdg));
        const double cross = e * std::cos(hdg) - n * std::sin(hdg);
        const float alongNm = (float)(along / kNm), crossNm = (float)(cross / kNm);
        if (alongNm < -0.2f || alongNm > out.fromNm) { havePrev = false; continue; }
        if (!p.hasAlt) continue;
        const float height = (float)(p.altFt - thrElev);
        if (alongNm <= out.fromNm && alongNm >= out.toNm) {
            const float want = (float)(alongNm * kNm * std::tan(glideDeg * track::kDeg) / track::kFt);
            const float dv = height - want;
            latSum += std::fabs(crossNm); vertSum += dv;
            out.maxLateralNm = std::max(out.maxLateralNm, std::fabs(crossNm));
            if (std::fabs(dv) > std::fabs(out.maxVerticalFt)) out.maxVerticalFt = dv;
            cnt++;
            if (std::fabs(crossNm) < 0.5f && alongNm > out.alignedFromNm) out.alignedFromNm = alongNm;
        }
        // where it crossed the threshold, interpolated between the two points either side
        if (havePrev && prevAlongNm > 0 && alongNm <= 0) {
            const float f = prevAlongNm / (prevAlongNm - alongNm);
            out.thresholdCrossFt = prevHeight + f * (height - prevHeight);
            out.crossed = true;
        }
        prevAlongNm = alongNm; prevHeight = height; havePrev = true;
    }
    if (cnt < 3) return false;
    out.ok = true;
    out.airport = a->icao;
    out.runway = ident;
    out.points = cnt;
    out.meanLateralNm = (float)(latSum / cnt);
    out.meanVerticalFt = (float)(vertSum / cnt);
    return true;
}

// ---------------------------------------------------------------- the path

std::string routeSignature(const std::vector<Point>& pts, double cellDeg) {
    if (pts.empty() || cellDeg <= 0) return std::string();
    std::string o;
    long lastA = LONG_MIN, lastB = LONG_MIN;
    char b[32];
    for (const Point& p : pts) {
        const long ca = (long)std::floor(p.lat / cellDeg), cb = (long)std::floor(wrap180(p.lon) / cellDeg);
        if (ca == lastA && cb == lastB) continue;
        lastA = ca; lastB = cb;
        snprintf(b, sizeof b, "%ld,%ld;", ca, cb);
        o += b;
        if (o.size() > 4000) break;   // a very long track: the start of it identifies the path well enough
    }
    return o;
}

float trackDistanceNm(const std::vector<Point>& a, const std::vector<Point>& b) {
    if (a.empty() || b.empty()) return 0;
    track::Frame fr;
    fr.set(a.front().lat, a.front().lon);
    auto local = [&](const std::vector<Point>& v) {
        std::vector<std::pair<double, double>> o;
        o.reserve(v.size());
        for (const Point& p : v) { double e = 0, n = 0; fr.toLocal(p.lat, p.lon, e, n); o.push_back({e, n}); }
        return o;
    };
    const auto la = local(a), lb = local(b);
    auto meanToOther = [](const std::vector<std::pair<double, double>>& from, const std::vector<std::pair<double, double>>& to) {
        double sum = 0;
        for (const auto& p : from) {
            double best = 1e18;
            if (to.size() == 1) best = std::hypot(p.first - to[0].first, p.second - to[0].second);
            for (size_t i = 0; i + 1 < to.size(); i++)
                best = std::min(best, pointSegM(p.first, p.second, to[i].first, to[i].second, to[i + 1].first, to[i + 1].second));
            sum += best;
        }
        return from.empty() ? 0.0 : sum / (double)from.size();
    };
    return (float)(std::max(meanToOther(la, lb), meanToOther(lb, la)) / kNm);
}

// ---------------------------------------------------------------- the kind of operation

bool militaryAddressBlock(uint32_t icao) {
    // US military: the block above the civil N-number range (ICAO Annex 10 Vol III, and what the FAA publishes)
    if (icao >= 0xADF7C8 && icao <= 0xAFFFFF) return true;
    // the blocks ICAO set aside for NATO and for aircraft of states that do not use a national block
    if (icao >= 0xF00000 && icao <= 0xF07FFF) return true;   // ICAO temporary / special
    if (icao >= 0xF09000 && icao <= 0xF093FF) return true;   // ICAO (2)
    return false;
}

bool airlineCallsign(const std::string& callsign, std::string& op) {
    const std::string c = upper(trim(callsign));
    if (c.size() < 4 || c.size() > 8) return false;
    for (int i = 0; i < 3; i++) if (!std::isalpha((unsigned char)c[(size_t)i])) return false;
    if (!std::isdigit((unsigned char)c[3])) return false;
    size_t i = 3;
    while (i < c.size() && std::isdigit((unsigned char)c[i])) i++;
    for (; i < c.size(); i++) if (!std::isalpha((unsigned char)c[i])) return false;   // a trailing letter or two is normal
    op = c.substr(0, 3);
    return true;
}

bool callsignIsRegistration(const std::string& callsign, const std::string& registration) {
    std::string c = upper(trim(callsign)), r = upper(trim(registration));
    const auto strip = [](std::string& s) { std::string o; for (char ch : s) if (ch != '-' && ch != ' ' && ch != '.') o += ch; s = o; };
    strip(c); strip(r);
    if (c.empty()) return false;
    if (!r.empty()) return c == r;
    // no registration known: an N-number shaped call sign is still its own registration
    if (c.size() < 2 || c.size() > 6 || c[0] != 'N' || !std::isdigit((unsigned char)c[1])) return false;
    for (size_t i = 1; i < c.size(); i++) if (!std::isalnum((unsigned char)c[i])) return false;
    return true;
}

namespace {
bool hasWord(const std::string& s, const char* const* words, size_t n) {
    const std::string u = upper(s);
    for (size_t i = 0; i < n; i++) if (u.find(words[i]) != std::string::npos) return true;
    return false;
}
const char* const kMedical[] = {"LIFE", "MEDIC", "MERCY", "ANGEL", "AIRMED", "AMBULANCE", "REACH", "STARFLIGHT", "AIRLIFT",
                                "HOSPITAL", "TRAUMA", "RESCUE", "SHOCK", "SAMARITAN", "CAREFLIGHT", "MEDFLIGHT", "AEROMED"};
const char* const kMilitary[] = {"REACH", "AIR FORCE", "AIRFORCE", "NAVY", "ARMY", "MARINE", "COAST GUARD", "COASTGUARD",
                                 "GUARD", "TANKER", "VIPER", "RAPTOR", "HOIST", "TALON", "EVAC", "MOBIL", "TROUT", "GOLD",
                                 "MILITARY", "DEFENCE", "DEFENSE", "GOVERNMENT"};
const char* const kSurvey[] = {"SURVEY", "PATROL", "PIPELINE", "TRAFFIC", "SKYWATCH", "AERIAL", "MAPPING", "PHOTO", "FIRE",
                               "SPOTTER", "SHERIFF", "POLICE", "BORDER", "CUSTOMS"};
} // namespace

bool medicalWord(const std::string& s) { return hasWord(s, kMedical, sizeof kMedical / sizeof *kMedical); }
bool militaryWord(const std::string& s) { return hasWord(s, kMilitary, sizeof kMilitary / sizeof *kMilitary); }
bool surveyWord(const std::string& s) { return hasWord(s, kSurvey, sizeof kSurvey / sizeof *kSurvey); }

int climbDropCycles(const std::vector<Point>& pts, int minClimbFt, float withinNm) {
    if (pts.size() < 6) return 0;
    // the aircraft has to stay near one spot for this to be a jump run rather than a journey
    double cLat = 0, cLon = 0; size_t n = 0;
    for (const Point& p : pts) { cLat += p.lat; cLon += p.lon; n++; }
    if (!n) return 0;
    cLat /= (double)n; cLon /= (double)n;
    for (const Point& p : pts) if (distNm(cLat, cLon, p.lat, p.lon) > withinNm) return 0;
    // count climbs of at least minClimbFt followed by a descent of as much
    int cycles = 0;
    int lowest = INT_MAX, highest = INT_MIN;
    bool climbing = true;
    for (const Point& p : pts) {
        if (!p.hasAlt) continue;
        if (lowest == INT_MAX) { lowest = highest = p.altFt; continue; }
        if (climbing) {
            if (p.altFt > highest) highest = p.altFt;
            if (highest - lowest >= minClimbFt && p.altFt < highest - minClimbFt / 2) { climbing = false; lowest = p.altFt; }
            else if (p.altFt < lowest) lowest = p.altFt;
        } else {
            if (p.altFt < lowest) lowest = p.altFt;
            if (highest - lowest >= minClimbFt && p.altFt > lowest + minClimbFt / 2) { cycles++; climbing = true; highest = p.altFt; }
        }
    }
    if (!climbing && highest - lowest >= minClimbFt) cycles++;   // the last descent counts
    return cycles;
}

int circuitCount(const std::vector<Point>& pts, float withinNm) {
    if (pts.size() < 6) return 0;
    // every return to the ground (or to a ground phase) at roughly the same place
    int touches = 0;
    bool wasGround = false;
    double gLat = 0, gLon = 0;
    bool haveFirst = false;
    for (const Point& p : pts) {
        const bool g = p.ground || p.phase == PhGround || p.phase == PhLanding;
        if (g && !wasGround) {
            if (!haveFirst) { gLat = p.lat; gLon = p.lon; haveFirst = true; touches++; }
            else if (distNm(gLat, gLon, p.lat, p.lon) <= withinNm) touches++;
        }
        wasGround = g;
    }
    return touches > 1 ? touches - 1 : 0;   // n touches at one field = n-1 circuits between them
}

KindGuess inferKind(const Flight& f, const std::vector<Point>& pts, const AircraftFacts& facts) {
    KindGuess g;
    const std::string cs = upper(trim(f.callsign));
    const std::string op = facts.operatorName;
    auto set = [&](OperationKind k, float conf, const std::string& why) {
        if (conf > g.confidence) { g.kind = k; g.confidence = conf; g.basis = why; }
    };

    // the shape of the flight: strong evidence, because it is what the aircraft actually did
    const int cycles = climbDropCycles(pts);
    if (cycles >= 2) set(OpSkydive, 0.8f, "climbed to altitude over one spot and came straight down " + std::to_string(cycles) + " times");
    else if (cycles == 1 && facts.category == "A1") set(OpSkydive, 0.45f, "one climb and drop over one spot in a light aircraft");
    const int circuits = circuitCount(pts);
    if (circuits >= 3) set(OpTraining, 0.7f, std::to_string(circuits) + " circuits at the same field");

    // the operator's name, where a database gave us one
    if (!op.empty()) {
        if (medicalWord(op)) set(OpMedical, 0.85f, "operator name \"" + op + "\"");
        if (militaryWord(op)) set(OpMilitary, 0.8f, "operator name \"" + op + "\"");
        if (surveyWord(op)) set(OpSurvey, 0.6f, "operator name \"" + op + "\"");
    }

    // the call sign
    if (!cs.empty()) {
        if (medicalWord(cs)) set(OpMedical, 0.7f, "call sign \"" + cs + "\"");
        if (militaryWord(cs)) set(OpMilitary, 0.6f, "call sign \"" + cs + "\"");
        if (surveyWord(cs)) set(OpSurvey, 0.5f, "call sign \"" + cs + "\"");
        if (cs.rfind("JUMP", 0) == 0) set(OpSkydive, 0.7f, "call sign \"" + cs + "\"");
        std::string opc;
        if (airlineCallsign(cs, opc)) set(OpAirline, 0.6f, "call sign \"" + cs + "\" is an operator code and a flight number");
        if (callsignIsRegistration(cs, facts.registration)) set(OpPrivate, 0.55f, "flying under its own registration \"" + cs + "\"");
    }

    // the address block
    if (militaryAddressBlock(facts.icao)) {
        char b[48];
        snprintf(b, sizeof b, "address %06X is in a military block", facts.icao);
        set(OpMilitary, 0.75f, b);
    }

    // a rotorcraft going between two places a few miles apart, low: a medical or police flight more often than not, but only a hint
    if (facts.rotorcraft && g.confidence < 0.4f && f.distanceNm > 3 && f.hasAlt && f.maxAltFt < 5000)
        set(OpUnknown, 0.0f, "");   // deliberately not guessed: too many kinds of helicopter fly like this

    // nothing at all to go on
    if (g.kind == OpUnknown && !facts.registration.empty() && cs.empty())
        set(OpPrivate, 0.25f, "no call sign, flying on a civil registration");
    return g;
}

// ---------------------------------------------------------------- the segmenter

struct Segmenter::Impl {
    SegmenterOptions opt;
    const AirportDb* db = nullptr;
    float endRadiusNm = 8;
    int minRunwayFt = 0;
    struct Open {
        Flight f;
        std::vector<Point> pts;
        double lastSeen = 0;
        double groundSince = -1;     // when it first reported being on the ground without flying since
        bool flewSinceGround = false;
        uint64_t messages0 = 0;      // the aircraft's message count when the flight began
        bool haveMsg0 = false;
    };
    std::unordered_map<uint32_t, Open> open;
    std::vector<Finished> done;
    double rxLat = 0, rxLon = 0;
    bool haveRx = false;

    explicit Impl(const SegmenterOptions& o) : opt(o) {}

    void addPoint(Open& o, const Point& p) {
        if (o.pts.size() >= opt.maxPoints) return;
        if (!o.pts.empty()) {
            const Point& last = o.pts.back();
            if (p.t - last.t < opt.pointMinSec && distNm(last.lat, last.lon, p.lat, p.lon) < opt.pointMinMoveNm) return;
        }
        o.pts.push_back(p);
    }

    void close(uint32_t icao, Open& o) {
        if (o.pts.size() >= opt.minPoints && o.f.tEnd - o.f.tStart >= opt.minFlightSec) {
            o.f.icao = icao;
            o.f.complete = true;
            // the field elevation to measure "near the ground" against, when the end of the flight was at a known airport
            int elev = -1000000;
            if (db) {
                const Airport* a = db->nearestWithRunway(o.pts.back().lat, o.pts.back().lon, endRadiusNm * kNm / 1000.0, minRunwayFt);
                if (!a) a = db->nearestWithRunway(o.pts.front().lat, o.pts.front().lon, endRadiusNm * kNm / 1000.0, minRunwayFt);
                if (a) elev = a->elevFt;
            }
            classifyPhases(o.pts, PhaseLimits(), elev);
            if (db) markApproachPhase(o.pts, *db);
            const double rx[2] = {rxLat, rxLon};
            summarise(o.f, o.pts, haveRx ? rx : nullptr);
            if (db) {
                findEnds(o.f, o.pts, *db, endRadiusNm, minRunwayFt);
                fitApproach(o.pts, *db, o.f.approach);
            }
            done.push_back({o.f, o.pts});
        }
    }
};

Segmenter::Segmenter(const SegmenterOptions& o) : p_(new Impl(o)) {}
Segmenter::~Segmenter() { delete p_; }

void Segmenter::setAirports(const AirportDb* db, float endRadiusNm, int minRunwayFt) {
    p_->db = db; p_->endRadiusNm = endRadiusNm; p_->minRunwayFt = minRunwayFt;
}
void Segmenter::setReceiver(double lat, double lon) { p_->rxLat = lat; p_->rxLon = lon; p_->haveRx = true; }
void Segmenter::clearReceiver() { p_->haveRx = false; }
size_t Segmenter::openFlights() const { return p_->open.size(); }

const Flight* Segmenter::open(uint32_t icao) const {
    const auto it = p_->open.find(icao);
    return it == p_->open.end() ? nullptr : &it->second.f;
}
const std::vector<Point>* Segmenter::openPoints(uint32_t icao) const {
    const auto it = p_->open.find(icao);
    return it == p_->open.end() ? nullptr : &it->second.pts;
}

void Segmenter::feedPoint(uint32_t icao, const Point& p, const std::string& callsign, const std::string& squawk) {
    Impl& m = *p_;
    auto it = m.open.find(icao);
    if (it != m.open.end() && p.t - it->second.lastSeen > m.opt.gapSec) { m.close(icao, it->second); m.open.erase(it); it = m.open.end(); }
    if (it == m.open.end()) {
        Impl::Open o;
        o.f.icao = icao;
        o.f.tStart = p.t;
        it = m.open.emplace(icao, std::move(o)).first;
    }
    Impl::Open& o = it->second;
    o.lastSeen = p.t;
    o.f.tEnd = p.t;
    if (!callsign.empty()) o.f.callsign = trim(callsign);
    if (!squawk.empty() && squawk != o.f.squawk) {
        o.f.squawk = squawk;
        if (std::find(o.f.squawksSeen.begin(), o.f.squawksSeen.end(), squawk) == o.f.squawksSeen.end()) o.f.squawksSeen.push_back(squawk);
        if (squawk == "7500" || squawk == "7600" || squawk == "7700") o.f.emergencySquawk = true;
    }
    if (p.lat || p.lon) m.addPoint(o, p);
    // a long stop on the ground after flying ends the flight
    if (p.ground) {
        if (o.groundSince < 0) o.groundSince = p.t;
        if (o.flewSinceGround && p.t - o.groundSince > m.opt.groundSplitSec) {
            m.close(icao, o);
            m.open.erase(it);
            return;
        }
    } else {
        o.groundSince = -1;
        o.flewSinceGround = true;
    }
}

void Segmenter::feed(const AdsbAircraft& a, double t) {
    Impl& m = *p_;
    Point p;
    p.t = t;
    p.hasAlt = a.hasAlt; p.altFt = a.altFt;
    p.hasSpeed = a.hasSpeed; p.speedKt = a.speedKt;
    p.hasVrate = a.hasVrate; p.vrateFpm = a.vrateFpm;
    p.ground = a.ground;
    if (a.hasHeading) { p.trackDeg = a.headingDeg; p.hasTrack = a.headingIsTrack; }
    if (a.hasPos) { p.lat = a.lat; p.lon = a.lon; }
    char sq[8] = {0};
    if (a.hasSquawk) snprintf(sq, sizeof sq, "%04d", a.squawk);
    feedPoint(a.icao, p, a.callsign, a.hasSquawk ? sq : "");
    const auto it = m.open.find(a.icao);
    if (it == m.open.end()) return;
    Impl::Open& o = it->second;
    if (!o.haveMsg0) { o.messages0 = a.messages; o.haveMsg0 = true; }
    o.f.messages = a.messages >= o.messages0 ? a.messages - o.messages0 : a.messages;
    if (a.emergency > o.f.emergency) o.f.emergency = a.emergency;
}

std::vector<Segmenter::Finished> Segmenter::take(double now) {
    Impl& m = *p_;
    for (auto it = m.open.begin(); it != m.open.end();) {
        if (now - it->second.lastSeen > m.opt.gapSec) { m.close(it->first, it->second); it = m.open.erase(it); }
        else ++it;
    }
    std::vector<Finished> out;
    out.swap(m.done);
    return out;
}

std::vector<Segmenter::Finished> Segmenter::flush() {
    Impl& m = *p_;
    for (auto& kv : m.open) m.close(kv.first, kv.second);
    m.open.clear();
    std::vector<Finished> out;
    out.swap(m.done);
    return out;
}

} // namespace dect2::flights
