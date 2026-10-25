// The flight history store, asked the questions a user actually has about their own airspace: what is overhead most, at what altitude and
// speed, on what days and at what hours, where from and where to, how many medical and military flights and where they go, which aircraft
// recur, the most active airports and operators, how regular an aircraft's flying is, what stands out, and the dossier the user builds by
// hand. Also the writing side: a flight and its track go in and come back unchanged, pruning, and the exports.
#include "dect2/flights/flights_db.h"
#include "dect2/flights/flights_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::flights;
#include "../common/test_util.h"

// A plausible flight without needing a radio: n points on a straight leg, with the figures filled in as summarise() would.
static void makeFlight(Flight& f, std::vector<Point>& pts, uint32_t icao, const char* callsign, double t0,
                       const char* org, const char* dst, int cruiseAlt, float cruiseSpeed, double minutes,
                       OperationKind kind = OpUnknown, float kindConf = 0, double lat0 = 40.0, double lon0 = -74.0, double brg = 90) {
    f = Flight();
    pts.clear();
    f.icao = icao;
    f.callsign = callsign;
    const int n = 20;
    for (int i = 0; i < n; i++) {
        Point p;
        p.t = t0 + (minutes * 60.0) * i / (n - 1);
        p.lat = lat0 + (brg == 0 ? i * 0.05 : 0);
        p.lon = lon0 + (brg == 90 ? i * 0.05 : 0);
        p.altFt = cruiseAlt; p.hasAlt = true;
        p.speedKt = cruiseSpeed; p.hasSpeed = true;
        p.phase = PhCruise;
        pts.push_back(p);
    }
    const double rx[2] = {40.0, -74.0};
    summarise(f, pts, rx);
    f.cruiseAltFt = cruiseAlt;
    f.maxAltFt = cruiseAlt;
    f.cruiseSpeedKt = cruiseSpeed;
    f.maxSpeedKt = cruiseSpeed;
    f.airborneSec = minutes * 60.0;
    if (org && *org) { f.origin.kind = EndKind::Airport; f.origin.icao = org; }
    if (dst && *dst) { f.destination.kind = EndKind::Airport; f.destination.icao = dst; }
    f.kind = (uint8_t)kind;
    f.kindConfidence = kindConf;
    f.complete = true;
}

static int localHourOf(double t) {
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    localtime_s(&g, &tt);
#else
    localtime_r(&tt, &g);
#endif
    return g.tm_hour;
}
static int localDowOf(double t) {
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    localtime_s(&g, &tt);
#else
    localtime_r(&tt, &g);
#endif
    return g.tm_wday;
}

static int64_t rankOf(const std::vector<Ranked>& v, const std::string& key) {
    for (const auto& r : v) if (r.key == key) return r.count;
    return -1;
}

int main() {
    Db db;
    std::string err;
    CHECK(db.open(":memory:", &err), "open: %s", err.c_str());
    CHECK(db.isOpen() && db.path() == ":memory:", "open state");

    // A fortnight of traffic over one receiver. Times are fixed so the test is the same every run.
    // 2026-06-01 12:00:00 UTC
    const double day0 = 1780315200.0;
    const uint32_t kMedevac = 0xA51001, kAirliner = 0xA51002, kPrivate = 0xA51003, kMilitary = 0xAE1004, kJumper = 0xA51005;
    Flight f;
    std::vector<Point> pts;

    // the medical helicopter: twice a day between its base and two hospitals, 14 days
    for (int d = 0; d < 14; d++) {
        makeFlight(f, pts, kMedevac, "LIFE1", day0 + d * 86400.0 + 3600, "KHOS", "KMCH", 2500, 130, 22, OpMedical, 0.85);
        AircraftFacts fa; fa.icao = kMedevac; fa.registration = "N911LF"; fa.type = "EC35"; fa.operatorName = "Life Flight Network";
        fa.category = "A7"; fa.rotorcraft = true;
        CHECK(db.addFlight(f, pts, fa), "medevac flight %d", d);
        makeFlight(f, pts, kMedevac, "LIFE1", day0 + d * 86400.0 + 7200, "KMCH", "KHOS", 2500, 130, 24, OpMedical, 0.85);
        db.addFlight(f, pts, fa);
    }
    // the airliner: once a day, same route, high and fast
    for (int d = 0; d < 14; d++) {
        makeFlight(f, pts, kAirliner, "DAL500", day0 + d * 86400.0 + 20000, "KJFK", "KLAX", 36000, 470, 310, OpAirline, 0.6);
        AircraftFacts fa; fa.icao = kAirliner; fa.registration = "N500DL"; fa.type = "B738"; fa.operatorName = "Delta Air Lines";
        fa.category = "A3";
        db.addFlight(f, pts, fa);
    }
    // a private aircraft, now and then, and once much further than usual
    for (int d = 0; d < 5; d++) {
        makeFlight(f, pts, kPrivate, "N77XY", day0 + d * 2 * 86400.0 + 50000, "KLDJ", "KLDJ", 4500, 120, 40, OpPrivate, 0.55);
        AircraftFacts fa; fa.icao = kPrivate; fa.registration = "N77XY"; fa.type = "C172";
        db.addFlight(f, pts, fa);
    }
    {   // the odd one out: a different path, far further, at an hour it never flies
        makeFlight(f, pts, kPrivate, "N77XY", day0 + 11 * 86400.0 + 3000, "KLDJ", "KBOS", 9500, 140, 180, OpPrivate, 0.55, 41.0, -72.0, 0);
        AircraftFacts fa; fa.icao = kPrivate; fa.registration = "N77XY"; fa.type = "C172";
        db.addFlight(f, pts, fa);
    }
    // a military flight, three times, from one base
    for (int d = 0; d < 3; d++) {
        makeFlight(f, pts, kMilitary, "RCH501", day0 + d * 4 * 86400.0 + 40000, "KWRI", "", 28000, 420, 95, OpMilitary, 0.75);
        AircraftFacts fa; fa.icao = kMilitary; fa.type = "C17";
        db.addFlight(f, pts, fa);
    }
    // the jump plane, weekends only, and one flight with an emergency squawk
    int jumpDays = 0;
    for (int d = 0; d < 14; d++) {
        if (localDowOf(day0 + d * 86400.0) != 6) continue;   // Saturdays
        for (int run = 0; run < 4; run++) {
            makeFlight(f, pts, kJumper, "JUMP1", day0 + d * 86400.0 + 30000 + run * 1800, "KSKY", "KSKY", 13000, 95, 25, OpSkydive, 0.8);
            AircraftFacts fa; fa.icao = kJumper; fa.registration = "N22JP"; fa.type = "C208";
            if (run == 2 && jumpDays == 0) { f.emergencySquawk = true; f.squawk = "7700"; f.squawksSeen = {"1200", "7700"}; }
            db.addFlight(f, pts, fa);
        }
        jumpDays++;
    }
    CHECK(jumpDays == 2, "%d Saturdays in the fortnight", jumpDays);

    const Stats st = db.stats();
    printf("  store: %lld flights, %lld points, %lld aircraft, %lld events\n",
           (long long)st.flights, (long long)st.points, (long long)st.aircraft, (long long)st.events);
    CHECK(st.flights == 28 + 14 + 6 + 3 + 8 && st.aircraft == 5, "%lld flights, %lld aircraft", (long long)st.flights, (long long)st.aircraft);
    CHECK(st.points == st.flights * 20, "%lld points for %lld flights", (long long)st.points, (long long)st.flights);

    // ---- "what plane is in my airspace the most?"
    {
        Filter all;
        const auto top = db.topAircraft(all, 10);
        CHECK(!top.empty(), "no aircraft ranked");
        printf("  most seen:\n");
        for (const auto& r : top) printf("    %s %-8s %lld flights, %.1f h airborne\n", r.key.c_str(), r.label.c_str(), (long long)r.count, r.seconds / 3600);
        CHECK(top[0].icao == kMedevac && top[0].count == 28, "the top aircraft is %s with %lld", top[0].key.c_str(), (long long)top[0].count);
        CHECK(top[0].label == "N911LF", "label '%s'", top[0].label.c_str());
        // "how many hours is that helicopter in the air?"
        CHECK(std::fabs(top[0].seconds - 14 * (22 + 24) * 60.0) < 1, "medevac airborne %.0f s", top[0].seconds);
    }

    // ---- "at what altitude and speed?" for one aircraft, and across everything
    {
        Filter one;
        one.icao = kAirliner;
        const auto alt = db.altitudeHistogram(one, 2000);
        const auto spd = db.speedHistogram(one, 25);
        CHECK(alt.size() == 1 && alt[0].bin == 36000 && alt[0].count == 14, "airliner altitude: %zu bands, first %d x%lld",
              alt.size(), alt.empty() ? -1 : alt[0].bin, alt.empty() ? 0 : (long long)alt[0].count);
        CHECK(spd.size() == 1 && spd[0].bin == 450 && spd[0].count == 14, "airliner speed band %d", spd.empty() ? -1 : spd[0].bin);
        Filter all;
        const auto altAll = db.altitudeHistogram(all, 5000);
        printf("  altitude bands (5000 ft): ");
        for (const auto& b : altAll) printf("%d:%lld ", b.bin, (long long)b.count);
        printf("\n");
        CHECK(altAll.size() >= 4, "%zu altitude bands over everything", altAll.size());
        AircraftRecord med;
        CHECK(db.aircraft(kMedevac, med), "no medevac dossier");
        CHECK(std::fabs(med.meanCruiseAltFt - 2500) < 1 && std::fabs(med.meanCruiseSpeedKt - 130) < 1,
              "medevac mean cruise %.0f ft / %.0f kt", med.meanCruiseAltFt, med.meanCruiseSpeedKt);
    }

    // ---- "what days of week and times do we see them?"
    {
        Filter j;
        j.icao = kJumper;
        const auto dow = db.weekdayHistogram(j);
        CHECK(dow.size() == 1 && dow[0].bin == 6, "the jump plane flew on %zu different weekdays (bin %d)", dow.size(), dow.empty() ? -1 : dow[0].bin);
        const auto hrs = db.hourHistogram(j);
        int64_t tot = 0;
        for (const auto& b : hrs) tot += b.count;
        CHECK(tot == 8, "jump plane hour histogram totals %lld", (long long)tot);
        // and the medevac flies every day
        Filter m;
        m.icao = kMedevac;
        CHECK(db.weekdayHistogram(m).size() == 7, "the medevac flew on %zu weekdays", db.weekdayHistogram(m).size());
        // filtering by hour finds the medevac's morning run only
        Filter mh = m;
        mh.minHour = mh.maxHour = localHourOf(day0 + 3600);
        CHECK(db.countFlights(mh) == 14, "%lld flights in the medevac's first hour", (long long)db.countFlights(mh));
    }

    // ---- "where are they going? where are they coming from?"
    {
        Filter m;
        m.icao = kMedevac;
        const auto routes = db.topRoutes(m, 10);
        printf("  medevac routes: ");
        for (const auto& r : routes) printf("[%s x%lld] ", r.key.c_str(), (long long)r.count);
        printf("\n");
        CHECK(routes.size() == 2 && routes[0].count == 14, "%zu routes", routes.size());
        CHECK(rankOf(routes, "KHOS - KMCH") == 14 && rankOf(routes, "KMCH - KHOS") == 14, "both legs present");
        // "what hospitals does it fly to" - the destinations of medical flights
        Filter all;
        const auto to = db.kindAirports(all, OpMedical, true, 10);
        const auto from = db.kindAirports(all, OpMedical, false, 10);
        CHECK(rankOf(to, "KMCH") == 14 && rankOf(to, "KHOS") == 14, "medical destinations");
        CHECK(rankOf(from, "KHOS") == 14, "medical origins");
        // "military: where are they going? where from?"
        const auto milFrom = db.kindAirports(all, OpMilitary, false, 10);
        CHECK(rankOf(milFrom, "KWRI") == 3, "military origins: %lld from KWRI", rankOf(milFrom, "KWRI"));
        const auto milTo = db.kindAirports(all, OpMilitary, true, 10);
        CHECK(milTo.empty(), "the military flights left range, so no destination should be named (%zu)", milTo.size());
    }

    // ---- "how many medivacs do we see?" and the other kinds
    {
        Filter all;
        const auto kinds = db.byKind(all);
        printf("  by kind: ");
        for (const auto& r : kinds) printf("%s=%lld ", r.label.c_str(), (long long)r.count);
        printf("\n");
        CHECK(rankOf(kinds, std::to_string((int)OpMedical)) == 28, "medical flights: %lld", rankOf(kinds, std::to_string((int)OpMedical)));
        CHECK(rankOf(kinds, std::to_string((int)OpMilitary)) == 3, "military flights");
        CHECK(rankOf(kinds, std::to_string((int)OpSkydive)) == 8, "skydive flights");
        CHECK(rankOf(kinds, std::to_string((int)OpAirline)) == 14, "airline flights");
        CHECK(rankOf(kinds, std::to_string((int)OpPrivate)) == 6, "private flights");
        bool labelled = false;
        for (const auto& r : kinds) if (r.label == "medical") labelled = true;
        CHECK(labelled, "the kinds are not labelled in words");
    }

    // ---- "most active airports / airlines / local private planes"
    {
        Filter all;
        const auto aps = db.topAirports(all, 10);
        printf("  busiest airports: ");
        for (const auto& r : aps) printf("%s=%lld ", r.key.c_str(), (long long)r.count);
        printf("\n");
        CHECK(rankOf(aps, "KHOS") == 28 && rankOf(aps, "KMCH") == 28, "the hospital pads lead: KHOS %lld", rankOf(aps, "KHOS"));
        CHECK(rankOf(aps, "KSKY") == 16, "the jump field counts both ends: %lld", rankOf(aps, "KSKY"));
        CHECK(rankOf(aps, "KJFK") == 14 && rankOf(aps, "KWRI") == 3, "KJFK and KWRI");
        const auto ops = db.topOperators(all, 10);
        CHECK(rankOf(ops, "DAL") == 14, "DAL flights: %lld", rankOf(ops, "DAL"));
        CHECK(rankOf(ops, "RCH") == 3, "RCH is an operator-code shaped call sign too: %lld", rankOf(ops, "RCH"));
        // private aircraft, most active first
        Filter priv;
        priv.kind = OpPrivate;
        const auto pa = db.topAircraft(priv, 10);
        CHECK(pa.size() == 1 && pa[0].icao == kPrivate && pa[0].count == 6, "%zu private aircraft", pa.size());
        const auto types = db.topTypes(all, 10);
        CHECK(rankOf(types, "EC35") == 28 && rankOf(types, "B738") == 14, "types: EC35 %lld", rankOf(types, "EC35"));
    }

    // ---- "do they fly a regular flight path?" and "recurring tail numbers"
    {
        const Regularity med = db.regularity(kMedevac);
        printf("  medevac regularity: %lld flights, %lld paths, commonest %lld (%.0f%%)\n",
               (long long)med.flights, (long long)med.distinctRoutes, (long long)med.commonestCount, 100 * med.share);
        CHECK(med.flights == 28 && med.distinctRoutes == 1, "the medevac flew the same ground path: %lld paths", (long long)med.distinctRoutes);
        CHECK(med.share > 0.99f, "share %.2f", med.share);
        const Regularity pv = db.regularity(kPrivate);
        CHECK(pv.distinctRoutes == 2 && pv.commonestCount == 5, "the private aircraft: %lld paths, commonest %lld",
              (long long)pv.distinctRoutes, (long long)pv.commonestCount);
        CHECK(pv.share > 0.8f && pv.share < 0.9f, "share %.2f", pv.share);
        // recurring: aircraft seen on many separate days
        Filter all;
        const auto top = db.topAircraft(all, 10);
        int recurring = 0;
        for (const auto& r : top) if (r.count >= 3) recurring++;
        CHECK(recurring == 5, "%d aircraft seen three times or more", recurring);
    }

    // ---- "is there an anomaly?" and "overall anomalies"
    {
        Filter all;
        const auto an = db.anomalies(all, 20);
        printf("  anomalies:\n");
        for (const auto& a : an) printf("    %s %-7s score %.1f - %s\n", a.registration.c_str(), a.callsign.c_str(), a.score, a.reason.c_str());
        CHECK(!an.empty(), "nothing stood out at all");
        bool emerg = false, offRoute = false;
        for (const auto& a : an) {
            if (a.icao == kJumper && a.reason.find("emergency") != std::string::npos) emerg = true;
            if (a.icao == kPrivate && a.reason.find("not flown before") != std::string::npos) offRoute = true;
        }
        CHECK(emerg, "the emergency squawk did not stand out");
        CHECK(offRoute, "the private aircraft's unusual trip did not stand out");
        CHECK(an[0].score >= 1.0f, "the emergency should rank first, top score %.1f", an[0].score);
        // the medevac, flying the same two legs every day, must not be called anomalous
        for (const auto& a : an) CHECK(a.icao != kMedevac, "the routine medevac was flagged: %s", a.reason.c_str());
    }

    // ---- "squawk information. emergencies."
    {
        Filter em;
        em.emergenciesOnly = true;
        const auto v = db.flights(em);
        CHECK(v.size() == 1 && v[0].icao == kJumper && v[0].squawk == "7700", "%zu emergency flights", v.size());
        CHECK(v.size() == 1 && v[0].squawksSeen.size() == 2, "squawks seen: %zu", v.empty() ? 0 : v[0].squawksSeen.size());
        const auto ev = db.events(Filter());
        int emergEvents = 0, firstEvents = 0;
        for (const auto& e : ev) { emergEvents += e.kind == "emergency"; firstEvents += e.kind == "first"; }
        CHECK(firstEvents == 5, "%d first-sighting events for 5 aircraft", firstEvents);
        CHECK(emergEvents == 1, "%d emergency events", emergEvents);
    }

    // ---- "more than usual?"
    {
        Filter med;
        med.kind = OpMedical;
        // the medevac flies twice a day every day: the last day should look exactly usual
        const Ranked r = db.rateVsUsual(med, 86400, 2);
        printf("  medical rate: %s, ratio %.2f\n", r.label.c_str(), r.value);
        // the window ends now, long after the fortnight, so there is nothing recent: the ratio must not pretend otherwise
        CHECK(r.count == 0, "%lld medical flights in the last day", (long long)r.count);
        // asked about a day inside the fortnight instead
        Filter in = med;
        in.until = day0 + 10 * 86400.0 + 86000;
        const Ranked r2 = db.rateVsUsual(in, 86400, 4);
        printf("  medical rate inside the fortnight: %s, ratio %.2f\n", r2.label.c_str(), r2.value);
        CHECK(r2.count == 2 && std::fabs(r2.value - 1.0) < 0.3, "%lld flights, ratio %.2f (should be about 1)", (long long)r2.count, r2.value);
    }

    // ---- "avg time on ground"
    {
        const auto gaps = db.groundGaps(kMedevac);
        CHECK(!gaps.empty(), "no ground gaps for the medevac");
        double sum = 0;
        for (double g : gaps) sum += g;
        const double mean = gaps.empty() ? 0 : sum / gaps.size();
        double shortest = 1e18, longest = 0;
        for (double g : gaps) { shortest = std::min(shortest, g); longest = std::max(longest, g); }
        printf("  medevac ground gaps: %zu, mean %.0f s, shortest %.0f s, longest %.0f s\n", gaps.size(), mean, shortest, longest);
        // Two stops a day: the turnaround at the hospital between the two legs, and overnight at its base.
        // Over a fortnight that is 14 turnarounds and 13 nights.
        CHECK(gaps.size() == 27, "%zu gaps", gaps.size());
        // the turnaround: it lands at KMCH 22 min after 3600 and leaves at 7200
        CHECK(std::fabs(shortest - (7200 - 3600 - 22 * 60)) < 60, "shortest ground time %.0f s", shortest);
        CHECK(longest > 20 * 3600 && longest < 24 * 3600, "the overnight stop is %.1f h", longest / 3600);
        AircraftRecord rec;
        db.aircraft(kMedevac, rec);
        CHECK(std::fabs(rec.meanGroundSec - mean) < 1, "the dossier says %.0f s", rec.meanGroundSec);
    }

    // ---- "their home airports", "private vs public", hours airborne
    {
        AircraftRecord rec;
        CHECK(db.aircraft(kMedevac, rec), "medevac dossier");
        printf("  medevac: %s %s (%s), %lld flights, %.1f h, home %s x%lld, kind %s\n", rec.registration.c_str(), rec.type.c_str(),
               rec.operatorName.c_str(), (long long)rec.flightCount, rec.airborneSec / 3600, rec.homeIcao.c_str(),
               (long long)rec.homeCount, operationKindName(rec.effectiveKind()));
        CHECK(rec.registration == "N911LF" && rec.type == "EC35" && rec.rotorcraft, "identity");
        CHECK(rec.flightCount == 28 && rec.homeCount == 28, "%lld flights, home count %lld", (long long)rec.flightCount, (long long)rec.homeCount);
        CHECK(rec.homeIcao == "KHOS" || rec.homeIcao == "KMCH", "home '%s'", rec.homeIcao.c_str());
        CHECK(rec.effectiveKind() == OpMedical && rec.kindConfidence > 0.8f, "kind %s", operationKindName(rec.effectiveKind()));
        CHECK(rec.firstSeen > 0 && rec.lastSeen > rec.firstSeen, "first and last seen");
        // hours airborne, for an estimate of time between inspections
        CHECK(std::fabs(rec.airborneSec / 3600.0 - 14 * 46 / 60.0) < 0.1, "airborne %.2f h", rec.airborneSec / 3600.0);
        AircraftRecord mil;
        db.aircraft(kMilitary, mil);
        CHECK(mil.effectiveKind() == OpMilitary && mil.registration.empty(), "military: kind %s, registration '%s'",
              operationKindName(mil.effectiveKind()), mil.registration.c_str());
    }

    // ---- the dossier the user builds: notes, tags, pictures, researched facts, and correcting the kind
    {
        CHECK(db.setNotes(kPrivate, "Based at the field east of town. Owner flies most Sundays."), "notes");
        CHECK(db.setTags(kPrivate, {"local", "watch"}), "tags");
        CHECK(db.setPictures(kPrivate, {"/home/user/pics/n77xy-1.jpg", "/home/user/pics/n77xy-2.jpg"}), "pictures");
        CHECK(db.setFact(kPrivate, "built", "1978"), "fact built");
        CHECK(db.setFact(kPrivate, "owner", "researched from the FAA registry"), "fact owner");
        CHECK(db.setFact(kPrivate, "temporary", "x") && db.setFact(kPrivate, "temporary", ""), "a fact removed again");
        AircraftRecord rec;
        CHECK(db.aircraft(kPrivate, rec), "private dossier");
        CHECK(rec.notes.find("east of town") != std::string::npos, "notes '%s'", rec.notes.c_str());
        CHECK(rec.tags.size() == 2 && rec.tags[0] == "local", "%zu tags", rec.tags.size());
        CHECK(rec.pictures.size() == 2 && rec.pictures[1].find("n77xy-2") != std::string::npos, "%zu pictures", rec.pictures.size());
        const auto fs = db.facts(kPrivate);
        CHECK(fs.size() == 2, "%zu facts after removing one", fs.size());
        bool built = false;
        for (const auto& x : fs) if (x.key == "built" && x.value == "1978") built = true;
        CHECK(built, "the researched year");
        // the user disagrees with the inference: their answer wins, and the program's is still there underneath
        CHECK(rec.effectiveKind() == OpPrivate, "before the override: %s", operationKindName(rec.effectiveKind()));
        CHECK(db.setKindOverride(kPrivate, OpSurvey), "override");
        db.aircraft(kPrivate, rec);
        CHECK(rec.effectiveKind() == OpSurvey && rec.kind == OpPrivate && rec.kindUser == OpSurvey,
              "after the override: effective %s, inferred %s", operationKindName(rec.effectiveKind()), operationKindName(rec.kind));
        // and the filter follows the override
        Filter sv;
        sv.kind = OpSurvey;
        CHECK(db.countFlights(sv) == 6, "%lld flights now count as survey", (long long)db.countFlights(sv));
        Filter pv;
        pv.kind = OpPrivate;
        CHECK(db.countFlights(pv) == 0, "%lld still count as private", (long long)db.countFlights(pv));
        CHECK(db.setKindOverride(kPrivate, -1), "clearing the override");
        db.aircraft(kPrivate, rec);
        CHECK(rec.effectiveKind() == OpPrivate && rec.kindUser == -1, "override cleared: %s", operationKindName(rec.effectiveKind()));
        // the user's own note on the timeline
        Event e;
        e.icao = kPrivate; e.t = day0; e.kind = "special"; e.text = "saw it do a low pass over the field"; e.user = true;
        CHECK(db.addEvent(e), "user event");
        Filter ef;
        ef.icao = kPrivate;
        const auto ev = db.events(ef);
        bool mine = false;
        int64_t id = 0;
        for (const auto& x : ev) if (x.user && x.kind == "special") { mine = true; id = x.id; }
        CHECK(mine, "the user's own event is not in the timeline");
        CHECK(db.removeEvent(id), "removing it");
        bool stillThere = false;
        for (const auto& x : db.events(ef)) if (x.id == id) stillThere = true;
        CHECK(!stillThere, "the event survived removal");
    }

    // ---- a flight and its track come back as they went in
    {
        Filter one;
        one.icao = kAirliner;
        one.limit = 1;
        const auto v = db.flights(one);
        CHECK(v.size() == 1, "%zu flights read back", v.size());
        if (v.size() == 1) {
            const Flight& g = v[0];
            CHECK(g.callsign == "DAL500" && g.cruiseAltFt == 36000 && g.maxSpeedKt == 470, "read back: %s %d ft %.0f kt",
                  g.callsign.c_str(), g.cruiseAltFt, g.maxSpeedKt);
            CHECK(g.origin.icao == "KJFK" && g.destination.icao == "KLAX" && g.origin.kind == EndKind::Airport, "ends read back");
            CHECK(g.points == 20 && g.id > 0, "%zu points, id %lld", g.points, (long long)g.id);
            const auto tr = db.track(g.id);
            CHECK(tr.size() == 20, "%zu track points", tr.size());
            CHECK(!tr.empty() && tr[0].hasAlt && tr[0].altFt == 36000 && tr[0].phase == PhCruise, "the track's values");
            CHECK(tr.size() > 1 && tr[1].t > tr[0].t, "the track is in order");
            Flight byId;
            CHECK(db.flight(g.id, byId) && byId.callsign == g.callsign, "read one flight by id");
        }
    }

    // ---- the exports
    {
        Filter one;
        one.icao = kMilitary;
        const std::string csv = db.exportFlights(one, "csv");
        const std::string json = db.exportFlights(one, "json");
        CHECK(csv.find("icao,callsign") == 0 && csv.find("AE1004") != std::string::npos && csv.find("RCH501") != std::string::npos, "flights csv");
        CHECK(csv.find("military") != std::string::npos, "the kind is in the csv");
        CHECK(json.find("\"callsign\":\"RCH501\"") != std::string::npos && json[0] == '[', "flights json");
        int lines = 0;
        for (size_t i = 0; i < csv.size(); i++) if (csv[i] == '\n') lines++;
        CHECK(lines == 4, "%d csv lines for 3 flights plus a header", lines);
        const auto v = db.flights(one);
        if (!v.empty()) {
            const std::string gpx = db.exportTrack(v[0].id, "gpx");
            const std::string tj = db.exportTrack(v[0].id, "json");
            const std::string tc = db.exportTrack(v[0].id, "csv");
            CHECK(gpx.find("<gpx") != std::string::npos && gpx.find("<trkpt") != std::string::npos && gpx.find("RCH501") != std::string::npos, "gpx");
            CHECK(tj.find("\"phase\":\"cruise\"") != std::string::npos, "track json");
            CHECK(tc.find("time,lat,lon") == 0, "track csv");
        }
    }

    // ---- pruning, and that a track goes with its flight
    {
        Db d2;
        CHECK(d2.open(":memory:"), "second store");
        for (int i = 0; i < 10; i++) {
            makeFlight(f, pts, 0xB00000 + (uint32_t)i, "T", day0 + i * 86400.0, "KAAA", "KBBB", 10000, 200, 30);
            AircraftFacts fa; fa.icao = 0xB00000 + (uint32_t)i;
            d2.addFlight(f, pts, fa);
        }
        CHECK(d2.stats().flights == 10 && d2.stats().points == 200, "%lld flights before pruning", (long long)d2.stats().flights);
        Limits l;
        l.maxFlights = 4;
        d2.setLimits(l);
        const int64_t gone = d2.prune();
        const Stats s2 = d2.stats();
        CHECK(gone == 6 && s2.flights == 4, "pruned %lld, %lld left", (long long)gone, (long long)s2.flights);
        CHECK(s2.points == 80, "%lld points left: the tracks must go with their flights", (long long)s2.points);
        // the oldest went, the newest stayed
        const auto left = d2.flights(Filter());
        CHECK(!left.empty() && left.front().tStart >= day0 + 6 * 86400.0, "the wrong end was pruned");
        // a store that keeps figures but not tracks
        Db d3;
        d3.open(":memory:");
        Limits nt;
        nt.keepTracks = false;
        d3.setLimits(nt);
        makeFlight(f, pts, 0xC00001, "NT", day0, "KAAA", "KBBB", 10000, 200, 30);
        AircraftFacts fa; fa.icao = 0xC00001;
        d3.addFlight(f, pts, fa);
        CHECK(d3.stats().flights == 1 && d3.stats().points == 0, "keepTracks off: %lld points", (long long)d3.stats().points);
        const auto nf = d3.flights(Filter());
        CHECK(nf.size() == 1 && nf[0].cruiseAltFt == 10000, "the figures survive without the track");
    }

    // ---- an empty store answers everything without falling over
    {
        Db e;
        CHECK(e.open(":memory:"), "empty store");
        Filter all;
        CHECK(e.flights(all).empty() && e.topAircraft(all).empty() && e.byKind(all).empty() && e.anomalies(all).empty(), "empty answers");
        CHECK(e.hourHistogram(all).empty() && e.topAirports(all).empty() && e.groundGaps(1).empty(), "empty distributions");
        AircraftRecord r;
        CHECK(!e.aircraft(0xABCDEF, r), "a dossier for an aircraft never heard");
        CHECK(e.countFlights(all) == 0 && e.stats().flights == 0, "counts");
        CHECK(e.regularity(1).flights == 0, "regularity of nothing");
        CHECK(e.rateVsUsual(all).count == 0, "rate of nothing");
        Flight bad;
        CHECK(!e.flight(999, bad), "a flight that does not exist");
        CHECK(e.track(999).empty(), "a track that does not exist");
    }
    return testSummary("flights_db");
}
