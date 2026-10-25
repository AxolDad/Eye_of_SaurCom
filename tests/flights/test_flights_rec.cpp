// The flight recorder (core/include/dect2/flights/flights_rec.h): what it writes and, above all, what it refuses to write.
//
// The recorder sits between the live aircraft table and the store, so it decides what the flight history is a record OF. The answer is "what
// this receiver heard", and the test that matters most here is the one proving an aircraft that reached the table some other way - another
// receiver's Beast or SBS feed, or an aggregator's API - never gets into the store.
#include "dect2/adsb/adsb_tel.h"
#include "dect2/flights/flights_db.h"
#include "dect2/flights/flights_rec.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::flights;
#include "../common/test_util.h"

namespace {

// A straightforward level flight past the receiver, one sample a second.
AdsbAircraft planeAt(uint32_t icao, int step, uint8_t srcId) {
    AdsbAircraft a;
    a.icao = icao;
    a.callsign = "TEST123";
    a.srcId = srcId;
    a.messages = (uint32_t)(step + 1) * 4;     // the recorder only looks again when this has moved
    a.ageSec = 0;
    a.hasPos = true;
    a.lat = 51.0 + step * 0.004;               // roughly 0.24 nm a second: about 860 kt, close enough for a track
    a.lon = -0.5;
    a.hasAlt = true;
    a.altFt = 20000;
    a.hasSpeed = true;
    a.speedKt = 420;
    a.hasHeading = true;
    a.headingDeg = 0;
    return a;
}

// Feeds `secs` seconds of one aircraft, then ends the flight by letting it go quiet and flushing.
void flyAndLand(Recorder& r, uint32_t icao, uint8_t srcId, int secs, double t0) {
    for (int i = 0; i < secs; i++) {
        AdsbTelemetry t;
        t.aircraft.push_back(planeAt(icao, i, srcId));
        r.feed(t, t0 + i);
    }
    r.flush();
}

}  // namespace

int main() {
    // ---- an aircraft this receiver heard is recorded
    {
        Db db;
        std::string err;
        CHECK(db.open(":memory:", &err), "could not open the store: %s", err.c_str());
        Recorder r;
        r.setStore(&db);
        flyAndLand(r, 0xABC001, 0, 60, 1760000000.0);
        CHECK(db.stats().flights == 1, "expected 1 flight from this receiver, got %lld", (long long)db.stats().flights);
        CHECK(r.status().flightsWritten == 1, "the recorder reported %lld written", (long long)r.status().flightsWritten);
        CHECK(db.stats().points > 0, "the flight has no track");
    }

    // ---- an aircraft that came from somewhere else is NOT recorded, however much of it arrives
    //
    // This is the whole point. The store answers "what is overhead most", "how far does the receiver reach", "how long was it in earshot".
    // An aircraft from another site's feed or from an aggregator never flew over this antenna, so letting it in would not add coverage, it
    // would quietly make those answers wrong - and nothing afterwards could tell the two apart.
    for (uint8_t src : {(uint8_t)1, (uint8_t)2, (uint8_t)7}) {
        Db db;
        CHECK(db.open(":memory:"), "could not open the store");
        Recorder r;
        r.setStore(&db);
        flyAndLand(r, 0xABC002, src, 120, 1760000000.0);
        CHECK(db.stats().flights == 0, "source %u put %lld flights in the store", src, (long long)db.stats().flights);
        CHECK(db.stats().points == 0, "source %u put %lld track points in the store", src, (long long)db.stats().points);
        CHECK(db.stats().aircraft == 0, "source %u created %lld dossiers", src, (long long)db.stats().aircraft);
        CHECK(r.status().flightsWritten == 0, "source %u had %lld flights written", src, (long long)r.status().flightsWritten);
    }

    // ---- the two together: only the local one survives, and it is the right one
    {
        Db db;
        CHECK(db.open(":memory:"), "could not open the store");
        Recorder r;
        r.setStore(&db);
        for (int i = 0; i < 60; i++) {
            AdsbTelemetry t;
            t.aircraft.push_back(planeAt(0xA10CAL, i, 0));   // heard here
            t.aircraft.push_back(planeAt(0xB1FEED, i, 1));   // arrived from a feed
            r.feed(t, 1760000000.0 + i);
        }
        r.flush();
        CHECK(db.stats().flights == 1, "expected only the local flight, got %lld", (long long)db.stats().flights);
        Filter f;
        const std::vector<Flight> v = db.flights(f);
        CHECK(v.size() == 1 && v[0].icao == 0xA10CAL, "the wrong aircraft was kept (%zu rows)", v.size());
        CHECK(db.stats().aircraft == 1, "%lld dossiers were made", (long long)db.stats().aircraft);
    }

    // ---- an aircraft whose message count never moves adds nothing after the first look
    {
        Db db;
        CHECK(db.open(":memory:"), "could not open the store");
        Recorder r;
        r.setStore(&db);
        AdsbAircraft a = planeAt(0xABC003, 0, 0);
        for (int i = 0; i < 100; i++) {   // same aircraft, same message count, moving position: a stale table entry
            AdsbTelemetry t;
            a.lat += 0.01;
            t.aircraft.push_back(a);
            r.feed(t, 1760000000.0 + i);
        }
        r.flush();
        CHECK(db.stats().points <= 1, "a stale entry contributed %lld points", (long long)db.stats().points);
    }

    // ---- recording off: nothing is written, and the open flight is dropped rather than kept
    {
        Db db;
        CHECK(db.open(":memory:"), "could not open the store");
        Recorder r;
        r.setStore(&db);
        r.setEnabled(false);
        CHECK(!r.enabled(), "setEnabled(false) did not take");
        flyAndLand(r, 0xABC004, 0, 60, 1760000000.0);
        CHECK(db.stats().flights == 0, "%lld flights were written while recording was off", (long long)db.stats().flights);
        r.setEnabled(true);
        flyAndLand(r, 0xABC005, 0, 60, 1760001000.0);
        CHECK(db.stats().flights == 1, "recording did not resume (%lld flights)", (long long)db.stats().flights);
    }

    // ---- with no store it does not fall over
    {
        Recorder r;
        flyAndLand(r, 0xABC006, 0, 10, 1760000000.0);
        CHECK(r.status().flightsWritten == 0, "something was written with no store attached");
    }

    // ---- an aircraft with no address is ignored
    {
        Db db;
        CHECK(db.open(":memory:"), "could not open the store");
        Recorder r;
        r.setStore(&db);
        for (int i = 0; i < 60; i++) {
            AdsbTelemetry t;
            AdsbAircraft a = planeAt(0, i, 0);
            t.aircraft.push_back(a);
            r.feed(t, 1760000000.0 + i);
        }
        r.flush();
        CHECK(db.stats().flights == 0, "an aircraft with no address made %lld flights", (long long)db.stats().flights);
    }

    // ---- the observation time is when the aircraft was heard, not when the pass happened
    {
        Db db;
        CHECK(db.open(":memory:"), "could not open the store");
        Recorder r;
        r.setStore(&db);
        const double t0 = 1760000000.0;
        for (int i = 0; i < 60; i++) {
            AdsbTelemetry t;
            AdsbAircraft a = planeAt(0xABC007, i, 0);
            a.ageSec = 5;                       // every report is five seconds stale
            t.aircraft.push_back(a);
            r.feed(t, t0 + i);
        }
        r.flush();
        Filter f;
        const std::vector<Flight> v = db.flights(f);
        CHECK(v.size() == 1, "expected one flight, got %zu", v.size());
        if (v.size() == 1)
            CHECK(std::fabs(v[0].tStart - (t0 - 5)) < 2, "the flight starts at %.0f, expected about %.0f", v[0].tStart, t0 - 5);
    }

    return testSummary("flights_rec");
}
