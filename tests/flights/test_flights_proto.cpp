// Flights out of the aircraft table: the phases of a whole flight, the figures it adds up to, where it began and ended, the approach
// measured against a real runway, the route signature and track comparison, the operation-kind inference, and the segmenter's splitting.
#include "dect2/airports/airports.h"
#include "dect2/flights/flights_proto.h"
#include "dect2/track/track_geo.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::flights;
#include "../common/test_util.h"

// a position a distance and bearing away from another
static void destPoint(double lat, double lon, double brgDeg, double nm, double& oLat, double& oLon) {
    const double d = nm * track::kNm / track::kEarthR, b = brgDeg * track::kDeg, p1 = lat * track::kDeg, l1 = lon * track::kDeg;
    const double sp = std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b);
    const double p2 = std::asin(sp);
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * sp);
    oLat = p2 / track::kDeg; oLon = track::wrap180(l2 / track::kDeg);
}

static Point pt(double t, double lat, double lon, int alt, float spd, int vr, bool ground = false) {
    Point p;
    p.t = t; p.lat = lat; p.lon = lon;
    p.altFt = alt; p.hasAlt = true;
    p.speedKt = spd; p.hasSpeed = true;
    p.vrateFpm = vr; p.hasVrate = true;
    p.ground = ground;
    return p;
}

// seconds spent in a phase, as a fraction of the flight
static double phaseFrac(const Flight& f, Phase ph) {
    uint32_t tot = 0;
    for (uint32_t s : f.phaseSec) tot += s;
    return tot ? (double)f.phaseSec[ph] / tot : 0;
}

int main() {
    const AirportDb& db = AirportDb::builtin();
    const Airport* jfk = db.find("KJFK");
    CHECK(jfk && !jfk->runways.empty(), "the test needs KJFK's runways");
    if (!jfk) return testSummary("flights_proto");

    // ---- a whole flight: roll at KJFK, climb, cruise, descend, approach 04L and land
    {
        // the 04L threshold and its heading
        const Runway* r04 = nullptr;
        for (const auto& r : jfk->runways) if (r.le.ident == "04L") r04 = &r;
        CHECK(r04 != nullptr, "KJFK 04L");
        const double thrLat = r04->le.latDeg, thrLon = r04->le.lonDeg, hdg = r04->le.headingDegT;
        std::vector<Point> pts;
        double t = 1760000000.0;   // a fixed Unix time so the test is the same every run
        // on the ground, then the roll
        for (int i = 0; i < 4; i++) { pts.push_back(pt(t, thrLat, thrLon, 13, 5, 0, true)); t += 5; }
        for (int i = 0; i < 3; i++) { pts.push_back(pt(t, thrLat, thrLon, 13, 60.f + 30 * i, 0, false)); t += 4; }
        // climb out along the runway heading to 35000 ft
        for (int i = 1; i <= 30; i++) {
            double la = 0, lo = 0;
            destPoint(thrLat, thrLon, hdg, i * 1.5, la, lo);
            pts.push_back(pt(t, la, lo, 13 + i * 1150, 250, 2200));
            t += 20;
        }
        // cruise 40 legs at 35000
        double cLat = 0, cLon = 0;
        destPoint(thrLat, thrLon, hdg, 45, cLat, cLon);
        for (int i = 0; i < 40; i++) {
            double la = 0, lo = 0;
            destPoint(cLat, cLon, 45, i * 4.0, la, lo);
            pts.push_back(pt(t, la, lo, 35000, 460, 0));
            t += 30;
        }
        // come back and descend, then fly the 04L centreline down a 3 degree path from 10 nm
        double dLat = 0, dLon = 0;
        destPoint(cLat, cLon, 45, 160, dLat, dLon);
        for (int i = 0; i < 20; i++) {
            double la = 0, lo = 0;
            destPoint(dLat, dLon, 225, i * 5.0, la, lo);
            pts.push_back(pt(t, la, lo, 35000 - i * 1500, 300, -1800));
            t += 25;
        }
        for (double d = 10.0; d >= 0.0; d -= 0.4) {
            double la = 0, lo = 0;
            destPoint(thrLat, thrLon, hdg + 180, d, la, lo);   // out in front of the threshold
            const int alt = (int)std::lround(13 + d * track::kNm * std::tan(3.0 * track::kDeg) / track::kFt);
            pts.push_back(pt(t, la, lo, alt, 140, -700));
            t += 10;
        }
        for (int i = 0; i < 5; i++) { pts.push_back(pt(t, thrLat, thrLon, 13, 30.f - 5 * i, 0, true)); t += 6; }

        classifyPhases(pts, PhaseLimits(), jfk->elevFt);
        markApproachPhase(pts, db);
        Flight f;
        const double rx[2] = {40.7, -73.8};
        summarise(f, pts, rx);
        findEnds(f, pts, db, 8, 5000);
        printf("  flight: %zu points, %.0f nm, max %d ft, cruise %d ft / %.0f kt, airborne %.0f s\n",
               f.points, f.distanceNm, f.maxAltFt, f.cruiseAltFt, f.cruiseSpeedKt, f.airborneSec);
        printf("  phase seconds: ground %u takeoff %u climb %u cruise %u descent %u approach %u landing %u\n",
               f.phaseSec[PhGround], f.phaseSec[PhTakeoff], f.phaseSec[PhClimb], f.phaseSec[PhCruise],
               f.phaseSec[PhDescent], f.phaseSec[PhApproach], f.phaseSec[PhLanding]);
        CHECK(f.maxAltFt == 35000 && f.cruiseAltFt == 35000, "max %d, cruise %d", f.maxAltFt, f.cruiseAltFt);
        CHECK(f.cruiseSpeedKt >= 450 && f.cruiseSpeedKt <= 470, "cruise speed %.0f", f.cruiseSpeedKt);
        CHECK(f.maxSpeedKt == 460, "max speed %.0f", f.maxSpeedKt);
        CHECK(f.sawGround && f.sawTakeoff && f.sawLanding, "ground %d takeoff %d landing %d", f.sawGround, f.sawTakeoff, f.sawLanding);
        CHECK(f.phaseSec[PhGround] > 0 && f.phaseSec[PhClimb] > 0 && f.phaseSec[PhCruise] > 0 && f.phaseSec[PhDescent] > 0, "every phase seen");
        CHECK(f.phaseSec[PhTakeoff] > 0 && f.phaseSec[PhLanding] > 0, "takeoff %u s, landing %u s", f.phaseSec[PhTakeoff], f.phaseSec[PhLanding]);
        CHECK(f.phaseSec[PhApproach] > 0, "the descent into KJFK was not marked as an approach");
        // the approach is the end of the descent, not all of it
        CHECK(f.phaseSec[PhApproach] < f.phaseSec[PhDescent], "approach %u s of %u s descent", f.phaseSec[PhApproach], f.phaseSec[PhDescent]);
        CHECK(f.airborneSec > 0 && f.airborneSec < f.tEnd - f.tStart, "airborne %.0f of %.0f s", f.airborneSec, f.tEnd - f.tStart);
        CHECK(f.distanceNm > 350 && f.distanceNm < 500, "distance %.0f nm", f.distanceNm);
        CHECK(f.hasRange && f.minRangeNm < 20, "range %.0f .. %.0f nm", f.minRangeNm, f.maxRangeNm);
        CHECK(f.maxVrateFpm == 2200 && f.minVrateFpm == -1800, "vrate %d .. %d", f.minVrateFpm, f.maxVrateFpm);
        // it began and ended on the ground at KJFK
        CHECK(f.origin.kind == EndKind::Airport && f.origin.icao == "KJFK", "origin %s %s", endKindName(f.origin.kind), f.origin.icao.c_str());
        CHECK(f.destination.kind == EndKind::Airport && f.destination.icao == "KJFK", "destination %s %s", endKindName(f.destination.kind), f.destination.icao.c_str());
        printf("  origin: %s %s rwy %s; destination: %s %s rwy %s\n", endKindName(f.origin.kind), f.origin.icao.c_str(), f.origin.runway.c_str(),
               endKindName(f.destination.kind), f.destination.icao.c_str(), f.destination.runway.c_str());

        // the approach: flown exactly on the centreline and the glide path, so both deviations are small
        ApproachFit fit;
        CHECK(fitApproach(pts, db, fit), "approach not fitted");
        if (fit.ok) {
            printf("  approach: %s rwy %s, %d points, lateral mean %.2f max %.2f nm, vertical mean %.0f max %.0f ft, threshold %.0f ft, aligned from %.1f nm\n",
                   fit.airport.c_str(), fit.runway.c_str(), fit.points, fit.meanLateralNm, fit.maxLateralNm, fit.meanVerticalFt, fit.maxVerticalFt,
                   fit.thresholdCrossFt, fit.alignedFromNm);
            CHECK(fit.airport == "KJFK" && fit.runway == "04L", "approach to %s %s", fit.airport.c_str(), fit.runway.c_str());
            CHECK(fit.meanLateralNm < 0.05f && fit.maxLateralNm < 0.1f, "lateral mean %.3f max %.3f nm on the centreline", fit.meanLateralNm, fit.maxLateralNm);
            CHECK(std::fabs(fit.meanVerticalFt) < 30 && std::fabs(fit.maxVerticalFt) < 60, "vertical mean %.0f max %.0f ft on a 3 degree path", fit.meanVerticalFt, fit.maxVerticalFt);
            CHECK(fit.crossed && fit.thresholdCrossFt > -20 && fit.thresholdCrossFt < 60, "crossed %d at %.0f ft", fit.crossed, fit.thresholdCrossFt);
            CHECK(fit.alignedFromNm > 8, "aligned from %.1f nm", fit.alignedFromNm);
        }
    }

    // ---- an approach flown high and off to one side is measured as such
    {
        const Runway* r04 = nullptr;
        for (const auto& r : jfk->runways) if (r.le.ident == "04L") r04 = &r;
        std::vector<Point> pts;
        double t = 1760100000.0;
        for (double d = 10.0; d >= 0.2; d -= 0.4) {
            double la = 0, lo = 0, oLat = 0, oLon = 0;
            destPoint(r04->le.latDeg, r04->le.lonDeg, r04->le.headingDegT + 180, d, la, lo);
            destPoint(la, lo, r04->le.headingDegT + 90, 0.6, oLat, oLon);   // 0.6 nm right of the centreline
            const int alt = (int)std::lround(13 + d * track::kNm * std::tan(3.0 * track::kDeg) / track::kFt) + 400;   // and 400 ft high
            pts.push_back(pt(t, oLat, oLon, alt, 150, -600));
            t += 10;
        }
        ApproachFit fit;
        CHECK(fitApproach(pts, db, fit), "offset approach not fitted");
        if (fit.ok) {
            printf("  offset approach: lateral mean %.2f nm, vertical mean %.0f ft, aligned from %.1f nm\n", fit.meanLateralNm, fit.meanVerticalFt, fit.alignedFromNm);
            CHECK(fit.meanLateralNm > 0.5f && fit.meanLateralNm < 0.75f, "lateral mean %.2f nm (0.6 flown)", fit.meanLateralNm);
            CHECK(fit.meanVerticalFt > 350 && fit.meanVerticalFt < 450, "vertical mean %.0f ft (400 flown)", fit.meanVerticalFt);
            CHECK(fit.alignedFromNm == 0, "never within half a mile, yet aligned from %.1f nm", fit.alignedFromNm);
        }
    }

    // ---- a flight that only passed overhead: no origin, no destination, no approach
    {
        std::vector<Point> pts;
        double t = 1760200000.0;
        for (int i = 0; i < 30; i++) {
            double la = 0, lo = 0;
            destPoint(41.5, -72.0, 90, i * 5.0, la, lo);
            pts.push_back(pt(t, la, lo, 37000, 470, 0));
            t += 30;
        }
        classifyPhases(pts);
        Flight f;
        summarise(f, pts, nullptr);
        findEnds(f, pts, db, 8, 5000);
        CHECK(f.origin.kind == EndKind::None && f.destination.kind == EndKind::None, "a flyover named an airport: %s / %s",
              f.origin.icao.c_str(), f.destination.icao.c_str());
        CHECK(phaseFrac(f, PhCruise) > 0.9, "a flyover is cruise: %.0f%%", 100 * phaseFrac(f, PhCruise));
        CHECK(!f.sawGround && !f.sawTakeoff && !f.sawLanding, "a flyover touched the ground");
        markApproachPhase(pts, db);
        Flight f2;
        summarise(f2, pts, nullptr);
        CHECK(f2.phaseSec[PhApproach] == 0, "a flyover gained %u s of approach", f2.phaseSec[PhApproach]);
        ApproachFit fit;
        CHECK(!fitApproach(pts, db, fit), "an approach was fitted to a flyover");
        CHECK(!f.hasRange, "range without a receiver position");
    }

    // ---- the route signature and comparing two tracks
    {
        std::vector<Point> a, b, c;
        double t = 1760300000.0;
        for (int i = 0; i < 20; i++) {
            double la = 0, lo = 0;
            destPoint(40.0, -74.0, 45, i * 3.0, la, lo);
            a.push_back(pt(t + i * 30, la, lo, 30000, 450, 0));
        }
        b = a;                                   // the same path again
        for (auto& p : b) p.t += 86400;          // a day later
        for (int i = 0; i < 20; i++) {           // a different path: the other way out of the same place
            double la = 0, lo = 0;
            destPoint(40.0, -74.0, 225, i * 3.0, la, lo);
            c.push_back(pt(t + i * 30, la, lo, 30000, 450, 0));
        }
        const std::string sa = routeSignature(a), sb = routeSignature(b), sc = routeSignature(c);
        CHECK(!sa.empty() && sa == sb, "the same path gave different signatures");
        CHECK(sa != sc, "opposite paths gave the same signature");
        CHECK(trackDistanceNm(a, b) < 0.01f, "the same path is %.3f nm apart", trackDistanceNm(a, b));
        CHECK(trackDistanceNm(a, c) > 10.f, "opposite paths are only %.1f nm apart", trackDistanceNm(a, c));
        // a track flown 2 nm to one side
        std::vector<Point> d;
        for (const Point& p : a) {
            double la = 0, lo = 0;
            destPoint(p.lat, p.lon, 135, 2.0, la, lo);
            Point q = p; q.lat = la; q.lon = lo;
            d.push_back(q);
        }
        const float off = trackDistanceNm(a, d);
        CHECK(off > 1.7f && off < 2.3f, "a 2 nm offset measured %.2f nm", off);
        printf("  tracks: same %.3f nm, 2 nm offset %.2f nm, opposite %.0f nm\n", trackDistanceNm(a, b), off, trackDistanceNm(a, c));
    }

    // ---- a parachute operation: climbs over one spot and comes straight down, twice
    {
        std::vector<Point> pts;
        double t = 1760400000.0;
        for (int run = 0; run < 2; run++) {
            for (int i = 0; i <= 14; i++) { pts.push_back(pt(t, 40.0 + 0.004 * (i % 3), -74.0, 1000 + i * 1000, 90, 1200)); t += 30; }
            for (int i = 14; i >= 0; i--) { pts.push_back(pt(t, 40.0 + 0.004 * (i % 3), -74.0, 1000 + i * 1000, 120, -3000)); t += 12; }
        }
        CHECK(climbDropCycles(pts) >= 2, "climb/drop cycles: %d", climbDropCycles(pts));
        classifyPhases(pts);
        Flight f;
        summarise(f, pts, nullptr);
        AircraftFacts facts;
        facts.icao = 0xA12345; facts.registration = "N123AB"; facts.category = "A1";
        const KindGuess g = inferKind(f, pts, facts);
        printf("  skydive: kind %s (%.2f) - %s\n", operationKindName(g.kind), g.confidence, g.basis.c_str());
        CHECK(g.kind == OpSkydive && g.confidence > 0.5f, "kind %s %.2f", operationKindName(g.kind), g.confidence);
        // a journey of the same shape but going somewhere is not a jump run
        std::vector<Point> trip;
        for (int i = 0; i < 40; i++) {
            double la = 0, lo = 0;
            destPoint(40.0, -74.0, 90, i * 4.0, la, lo);
            trip.push_back(pt(1760500000.0 + i * 30, la, lo, 2000 + (i < 20 ? i * 1500 : (40 - i) * 1500), 300, i < 20 ? 1500 : -1500));
        }
        CHECK(climbDropCycles(trip) == 0, "a cross-country climb and descent counted as %d jump runs", climbDropCycles(trip));
    }

    // ---- circuits: four touches at one field
    {
        std::vector<Point> pts;
        double t = 1760600000.0;
        for (int lap = 0; lap < 4; lap++) {
            pts.push_back(pt(t, 40.0, -74.0, 20, 10, 0, true)); t += 20;
            for (int i = 1; i <= 6; i++) { double la = 0, lo = 0; destPoint(40.0, -74.0, 90, i * 0.4, la, lo); pts.push_back(pt(t, la, lo, 20 + i * 200, 90, 700)); t += 20; }
            for (int i = 6; i >= 1; i--) { double la = 0, lo = 0; destPoint(40.0, -74.0, 270, i * 0.4, la, lo); pts.push_back(pt(t, la, lo, 20 + i * 200, 80, -700)); t += 20; }
        }
        pts.push_back(pt(t, 40.0, -74.0, 20, 8, 0, true));
        classifyPhases(pts);
        const int c = circuitCount(pts);
        CHECK(c >= 3, "circuits counted: %d", c);
        Flight f;
        summarise(f, pts, nullptr);
        AircraftFacts facts;
        facts.icao = 0xA22222; facts.registration = "N55TT"; facts.category = "A1";
        const KindGuess g = inferKind(f, pts, facts);
        printf("  circuits: %d, kind %s (%.2f) - %s\n", c, operationKindName(g.kind), g.confidence, g.basis.c_str());
        CHECK(g.kind == OpTraining, "kind %s", operationKindName(g.kind));
    }

    // ---- what the call signs, operators and addresses say
    {
        std::string op;
        CHECK(airlineCallsign("DAL123", op) && op == "DAL", "DAL123 -> %s", op.c_str());
        CHECK(airlineCallsign("BAW22AB", op) && op == "BAW", "BAW22AB -> %s", op.c_str());
        CHECK(!airlineCallsign("N123AB", op), "N123AB taken for an airline");
        CHECK(!airlineCallsign("AB1", op) && !airlineCallsign("", op) && !airlineCallsign("TOOLONGONE", op), "short, empty and long call signs");
        CHECK(callsignIsRegistration("N123AB", "N123AB") && callsignIsRegistration("N123AB", ""), "a registration call sign");
        CHECK(callsignIsRegistration("C-GABC", "CGABC"), "a dashed registration");
        CHECK(!callsignIsRegistration("DAL123", "N123AB") && !callsignIsRegistration("", ""), "not registrations");
        CHECK(medicalWord("LIFEFLIGHT 2") && medicalWord("Mercy Air") && !medicalWord("DAL123"), "medical words");
        CHECK(militaryWord("Air Force") && militaryWord("RCH501") == false, "military words: RCH501 has no word, the address block catches it");
        CHECK(surveyWord("Pipeline Patrol") && !surveyWord("United Airlines"), "survey words");
        CHECK(militaryAddressBlock(0xAE1234) && !militaryAddressBlock(0xA12345), "the US military block");

        Flight f;
        std::vector<Point> none;
        AircraftFacts air;
        air.icao = 0x4CA123; air.registration = "EI-ABC"; air.operatorName = "Ryanair";
        f.callsign = "RYR1234";
        KindGuess g = inferKind(f, none, air);
        CHECK(g.kind == OpAirline, "RYR1234 -> %s", operationKindName(g.kind));
        AircraftFacts med;
        med.icao = 0xA33333; med.registration = "N911LF"; med.operatorName = "Life Flight Network"; med.category = "A7"; med.rotorcraft = true;
        f.callsign = "LIFE1";
        g = inferKind(f, none, med);
        CHECK(g.kind == OpMedical && g.confidence > 0.7f, "LIFE1 -> %s %.2f", operationKindName(g.kind), g.confidence);
        printf("  medical: %s (%.2f) - %s\n", operationKindName(g.kind), g.confidence, g.basis.c_str());
        AircraftFacts mil;
        mil.icao = 0xAE0123;
        f.callsign = "RCH501";
        g = inferKind(f, none, mil);
        CHECK(g.kind == OpMilitary && g.basis.find("AE0123") != std::string::npos, "RCH501 -> %s (%s)", operationKindName(g.kind), g.basis.c_str());
        printf("  military: %s (%.2f) - %s\n", operationKindName(g.kind), g.confidence, g.basis.c_str());
        AircraftFacts priv;
        priv.icao = 0xA44444; priv.registration = "N77XY";
        f.callsign = "N77XY";
        g = inferKind(f, none, priv);
        CHECK(g.kind == OpPrivate, "N77XY -> %s", operationKindName(g.kind));
        // nothing at all to go on
        AircraftFacts blank;
        blank.icao = 0x3C1234;
        f.callsign = "";
        g = inferKind(f, none, blank);
        CHECK(g.kind == OpUnknown && g.confidence == 0, "an unknown aircraft guessed %s %.2f", operationKindName(g.kind), g.confidence);
    }

    // ---- the segmenter: silence splits flights, a long stop on the ground splits them, short ones are dropped
    {
        Segmenter seg;
        seg.setReceiver(40.7, -73.8);
        double t = 1760700000.0;
        // flight one: 20 points over 10 minutes
        for (int i = 0; i < 20; i++) {
            double la = 0, lo = 0;
            destPoint(40.5, -74.0, 90, i * 2.0, la, lo);
            seg.feedPoint(0xABC123, pt(t + i * 30, la, lo, 20000, 400, 0), "TST1", "1200");
        }
        CHECK(seg.openFlights() == 1 && seg.open(0xABC123) != nullptr, "one open flight");
        CHECK(seg.take(t + 20 * 30).empty(), "a flight ended while still being heard");
        // ten minutes of silence ends it
        auto done = seg.take(t + 20 * 30 + 601);
        CHECK(done.size() == 1, "%zu flights after the silence", done.size());
        if (done.size() == 1) {
            CHECK(done[0].f.icao == 0xABC123 && done[0].f.callsign == "TST1" && done[0].f.squawk == "1200", "identity carried through");
            CHECK(done[0].f.complete && done[0].pts.size() == 20, "%zu points, complete %d", done[0].pts.size(), done[0].f.complete);
            CHECK(done[0].f.hasRange && done[0].f.maxRangeNm > 10, "range %.0f nm", done[0].f.maxRangeNm);
            CHECK(done[0].pts[0].phase != PhUnknown, "phases were not classified");
        }
        CHECK(seg.openFlights() == 0, "the flight stayed open");
        // a second flight from the same aircraft is its own session
        for (int i = 0; i < 20; i++) {
            double la = 0, lo = 0;
            destPoint(41.0, -74.0, 180, i * 2.0, la, lo);
            seg.feedPoint(0xABC123, pt(t + 4000 + i * 30, la, lo, 15000, 350, 0), "TST2");
        }
        auto d2 = seg.flush();
        CHECK(d2.size() == 1 && d2[0].f.callsign == "TST2", "%zu flights, call sign %s", d2.size(), d2.empty() ? "" : d2[0].f.callsign.c_str());
        // too short to be a flight
        Segmenter s2;
        s2.feedPoint(0xDDD111, pt(t, 40.0, -74.0, 1000, 100, 0));
        s2.feedPoint(0xDDD111, pt(t + 2, 40.001, -74.0, 1000, 100, 0));
        CHECK(s2.flush().empty(), "a two point flight was kept");
        // a long stop on the ground after flying ends the flight
        Segmenter s3;
        double u = 1760800000.0;
        for (int i = 0; i < 10; i++) { double la = 0, lo = 0; destPoint(40.0, -74.0, 90, i * 1.0, la, lo); s3.feedPoint(0xEEE222, pt(u + i * 20, la, lo, 5000, 200, 0)); }
        u += 200;
        for (int i = 0; i < 6; i++) s3.feedPoint(0xEEE222, pt(u + i * 200, 40.2, -74.0, 20, 5, 0, true));   // on the ground for 1000 s
        auto d3 = s3.take(u + 1000);
        CHECK(d3.size() == 1 && d3[0].f.sawGround, "%zu flights after a long stop on the ground", d3.size());
        // thinning: points closer than a second that have not moved are left out
        Segmenter s4;
        double w = 1760900000.0;
        for (int i = 0; i < 100; i++) s4.feedPoint(0xFFF333, pt(w + i * 0.2, 40.0, -74.0, 3000, 150, 0));
        for (int i = 0; i < 10; i++) { double la = 0, lo = 0; destPoint(40.0, -74.0, 90, 1.0 + i, la, lo); s4.feedPoint(0xFFF333, pt(w + 30 + i * 10, la, lo, 3000, 150, 0)); }
        auto d4 = s4.flush();
        CHECK(d4.size() == 1 && d4[0].pts.size() <= 31, "thinning kept %zu of 110 points (a point a second while stationary, plus the 10 moving)", d4.empty() ? 0 : d4[0].pts.size());
        // an emergency squawk is remembered
        Segmenter s5;
        double v = 1761000000.0;
        for (int i = 0; i < 10; i++) { double la = 0, lo = 0; destPoint(40.0, -74.0, 90, i * 2.0, la, lo); s5.feedPoint(0xAAA444, pt(v + i * 30, la, lo, 10000, 250, 0), "EMG1", i == 5 ? "7700" : "1200"); }
        auto d5 = s5.flush();
        CHECK(d5.size() == 1 && d5[0].f.emergencySquawk, "the 7700 was not remembered");
        CHECK(d5.size() == 1 && d5[0].f.squawksSeen.size() == 2, "%zu squawks seen", d5.empty() ? 0 : d5[0].f.squawksSeen.size());
    }

    // ---- phases of a flight with no vertical rate reported: differentiated from the altitudes
    {
        std::vector<Point> pts;
        double t = 1761100000.0;
        for (int i = 0; i < 30; i++) {
            double la = 0, lo = 0;
            destPoint(40.0, -74.0, 90, i * 2.0, la, lo);
            Point p = pt(t + i * 20, la, lo, i < 10 ? 2000 + i * 2000 : i < 20 ? 20000 : 20000 - (i - 20) * 2000, 300, 0);
            p.hasVrate = false;
            pts.push_back(p);
        }
        classifyPhases(pts);
        int climb = 0, cruise = 0, desc = 0;
        for (const Point& p : pts) { climb += p.phase == PhClimb; cruise += p.phase == PhCruise; desc += p.phase == PhDescent; }
        printf("  no vrate: %d climb, %d cruise, %d descent\n", climb, cruise, desc);
        CHECK(climb > 5 && cruise > 5 && desc > 5, "climb %d cruise %d descent %d from altitudes alone", climb, cruise, desc);
    }
    return testSummary("flights_proto");
}
