// The Eye's live model (core/src/eye/eye.cpp): groups, first sightings and new messages in the timeline, position updates that only move the
// contact, voice and encrypted content left out, filters, per-group counts, the limits, and the engine hook that feeds it.
#include "dect2/automation/engine.h"
#include "dect2/eye/eye.h"
#include "../common/test_util.h"
#include <chrono>
#include <thread>

using namespace dect2::eye;
namespace au = dect2::automation;

namespace {
constexpr double kT0 = 1792232100.0;

au::Event aircraft(const char* hex, const char* callsign, double t, double lat, double lon) {
    au::Event e("adsb.aircraft");
    e.at(t, "ADS-B", 1090e6);
    e.str("hex", hex).str("callsign", callsign).num("alt_ft", 35000).num("lat", lat).num("lon", lon);
    return e;
}

au::Event pager(double ric, const char* text, double t) {
    au::Event e("pager.message");
    e.at(t, "POCSAG", 152e6);
    e.num("address", ric).str("text", text).str("protocol", "POCSAG");
    return e;
}

au::Event vessel(double mmsi, const char* name, double t) {
    au::Event e("ais.vessel");
    e.at(t, "AIS", 162e6);
    e.num("mmsi", mmsi).str("name", name).num("lat", 55.1).num("lon", 21.2);
    return e;
}
}  // namespace

int main() {
    // groups
    CHECK(groupOfType("adsb.aircraft") == GAircraft && groupOfType("acars.message") == GAirData && groupOfType("hfdl.message") == GAirData, "air groups");
    CHECK(groupOfType("ais.vessel") == GShips && groupOfType("dsc.call") == GShips && groupOfType("lora.packet") == GMesh, "ships and mesh");
    CHECK(groupOfType("signal.level") == -1 && groupOfType("gnss.fix") == -1 && groupOfType("nonsense") == -1, "types the Eye leaves out");
    CHECK(std::string(groupName(GPagers)) == "Pagers" && std::string(groupName(99)).empty(), "names");
    CHECK((familyMask() >> au::FAdsb & 1) && (familyMask() >> au::FDsc & 1) && !(familyMask() >> au::FSignal & 1), "family mask");

    // an aircraft reporting every 2 s: one "first seen" row, the contact moves
    Overview o;
    for (int i = 0; i < 10; i++) o.ingest(aircraft("4CA2D1", "RYR4LK", kT0 + 2 * i, 54.0 + 0.01 * i, 25.0));
    auto rows = o.timeline(100);
    CHECK(rows.size() == 1 && rows[0].first && rows[0].text == "first seen", "one row for ten position updates: %zu", rows.size());
    CHECK(rows.size() == 1 && rows[0].label == "RYR4LK" && rows[0].ident == "4CA2D1" && rows[0].group == GAircraft, "row identity");
    auto cs = o.contacts(kT0 + 20, 60);
    CHECK(cs.size() == 1 && cs[0].events == 10 && cs[0].hasPos && std::abs(cs[0].lat - 54.09) < 1e-9, "contact moved to the last position");

    // pager messages: every new text is a row, a repeat is not
    o.ingest(pager(1234567, "FIRE ALARM MAIN ST 12", kT0 + 30));
    o.ingest(pager(1234567, "FIRE ALARM MAIN ST 12", kT0 + 31));
    o.ingest(pager(1234567, "ALL CLEAR", kT0 + 40));
    rows = o.timeline(100);
    CHECK(rows.size() == 3 && rows[0].text == "ALL CLEAR" && rows[1].text == "FIRE ALARM MAIN ST 12" && rows[1].first, "pager rows, newest first: %zu", rows.size());
    CHECK(rows.size() == 3 && rows[0].ident == "1234567" && !rows[0].first, "pager ident and a later row");

    // a ship: its name is the label, not a message
    o.ingest(vessel(219012345, "NORDIC STAR", kT0 + 50));
    o.ingest(vessel(219012345, "NORDIC STAR", kT0 + 60));
    rows = o.timeline(100, 1u << GShips);
    CHECK(rows.size() == 1 && rows[0].label == "NORDIC STAR" && rows[0].text == "first seen", "ship rows: %zu", rows.size());

    // voice and encrypted content stay out
    au::Event enc = pager(7654321, "SECRET", kT0 + 70);
    enc.flag("encrypted", true);
    o.ingest(enc);
    CHECK(o.timeline(100, 0, "secret").empty() && o.contacts(kT0 + 70, 60, 1u << GPagers).size() == 1, "encrypted left out");
    au::Event other("signal.level");
    other.at(kT0 + 71, "General", 100e6);
    o.ingest(other);
    CHECK(o.total() == 15, "only Eye types counted: %llu", (unsigned long long)o.total());

    // filters
    CHECK(o.timeline(100, 0, "ryr4lk").size() == 1 && o.timeline(100, 0, "pocsag").size() == 2 && o.timeline(100, 0, "nothing").empty(), "text filter");
    CHECK(o.timeline(100, (1u << GAircraft) | (1u << GShips)).size() == 2, "group mask");
    CHECK(o.timeline(1).size() == 1, "max rows");

    // counts per group over a window
    auto st = o.stats(kT0 + 60, 30);
    CHECK(st[GPagers].active == 1 && st[GPagers].events == 2 && st[GShips].active == 1 && st[GShips].events == 2, "last 30 s: pagers %d/%d ships %d/%d",
          st[GPagers].active, st[GPagers].events, st[GShips].active, st[GShips].events);
    CHECK(st[GAircraft].active == 0, "aircraft quiet for 40 s");
    st = o.stats(kT0 + 60, 3600);
    CHECK(st[GAircraft].active == 1 && st[GAircraft].events == 10, "the hour");
    CHECK(o.contacts(kT0 + 60, 3600).front().ident == "219012345", "most recent first");
    CHECK(o.contacts(kT0 + 60, 3600, 0, "nordic").size() == 1 && o.contacts(kT0 + 60, 3600, 0, "zzz").empty(), "contact filter");
    const uint64_t v0 = o.version();
    o.ingest(pager(1234567, "ALL CLEAR", kT0 + 61));
    CHECK(o.version() != v0, "version moves with an event");

    // limits: rows and contacts
    Limits lim;
    lim.maxRows = 5;
    lim.maxContacts = 3;
    Overview capped(lim);
    for (int i = 0; i < 8; i++) capped.ingest(pager(100 + i, "MSG", kT0 + i));
    CHECK(capped.timeline(100).size() == 5 && capped.timeline(100)[0].ident == "107", "row cap keeps the newest");
    auto sc = capped.contacts(kT0 + 10, 3600);
    CHECK(sc.size() == 3 && sc.back().ident == "105", "contact cap forgets the least recently heard: %zu", sc.size());
    capped.clear();
    CHECK(capped.timeline(100).empty() && capped.total() == 0, "clear");

    // the engine hook: an Eye hook makes the decoders' families wanted, and sees their events on submit
    au::Engine eng;
    Overview fed;
    CHECK(!eng.wants(au::FPager), "nothing wanted before the hook");
    eng.setOverview([&fed](const au::Event& e) { fed.ingest(e); }, familyMask());
    CHECK(eng.wants(au::FPager) && eng.wants(au::FAdsb) && !eng.wants(au::FGnss), "hook families wanted");
    eng.submit(pager(42, "HELLO", kT0));
    CHECK(fed.timeline(10).size() == 1, "hook saw the event");
    eng.setOverview(nullptr, 0);
    CHECK(!eng.wants(au::FPager), "hook removed");

    // one picture across sites: the same ident from the local station and from site "b" is one contact with both sites
    {
        Overview m;
        au::Event local = aircraft("4CA2D1", "RYR4LK", kT0, 54.0, 25.0);
        au::Event remote = aircraft("4CA2D1", "RYR4LK", kT0 + 1, 54.1, 25.1);
        remote.str("site", "b");
        au::Event only = aircraft("AAAAAA", "ONLYB", kT0 + 2, 50.0, 20.0);
        only.str("site", "b");
        m.ingest(local); m.ingest(remote); m.ingest(only);
        auto mc = m.contacts(kT0 + 5, 60);
        const Contact* c = nullptr;
        for (const auto& k : mc) if (k.ident == "4CA2D1") c = &k;
        CHECK(mc.size() == 2 && c && c->sites == std::vector<std::string>({"", "b"}), "one contact, both sites");
        auto tb = m.timeline(100, 0, "", "b");
        CHECK(tb.size() == 2 && tb[0].site == "b" && tb[1].site == "b", "timeline of site b: %zu", tb.size());
        CHECK(m.timeline(100).size() == 3, "all sites: one local row, two from b");
        CHECK(m.contacts(kT0 + 5, 60, 0, "", "b").size() == 2 && m.contacts(kT0 + 5, 60, 0, "", "zz").empty(), "contacts of a site");
        auto tl = m.timeline(100, 0, "", "");
        bool anyLocal = false;
        for (const auto& r : tl) if (r.site.empty()) anyLocal = true;
        CHECK(anyLocal, "local rows have an empty site");
    }

    // located signals: one contact per frequency, a row only the first time or when the text changes
    {
        Overview m;
        CHECK(std::string(groupName(GFixes)) == "Located", "Located group name");
        m.ingestLocated(kT0, 433.92e6, 54.7, 25.3, 120.4, {"a", "b"});
        m.ingestLocated(kT0 + 1, 433.92e6, 54.7001, 25.3001, 120.4, {"a", "b"});
        auto rows2 = m.timeline(100, 1u << GFixes);
        CHECK(rows2.size() == 1 && rows2[0].ident == "433920" && rows2[0].text == "located by a, b, \xC2\xB1" "120 m", "located row: %zu '%s'", rows2.size(), rows2.empty() ? "" : rows2[0].text.c_str());
        auto fc = m.contacts(kT0 + 2, 60, 1u << GFixes);
        CHECK(fc.size() == 1 && fc[0].hasPos && std::abs(fc[0].lat - 54.7001) < 1e-9 && fc[0].label == "433.920 MHz", "located contact moved: %zu", fc.size());
        m.ingestLocated(kT0 + 3, 433.92e6, 54.7, 25.3, 300, {"a", "b"});
        CHECK(m.timeline(100, 1u << GFixes).size() == 2, "a clearly larger error is a new row");
    }
    {   // jittered live fixes of one transmitter: one row; a third site is a second
        Overview m;
        for (int i = 0; i < 10; i++)
            m.ingestLocated(kT0 + i, 433.92e6, 54.7 + ((i % 3) - 1) * 0.00005, 25.3 + ((i % 2) ? 1 : -1) * 0.00005, 120 * (1 + 0.05 * ((i % 3) - 1)), {"a", "b"});
        CHECK(m.timeline(100, 1u << GFixes).size() == 1, "jitter is one row: %zu", m.timeline(100, 1u << GFixes).size());
        CHECK(m.total() == 1 && m.stats(kT0 + 10, 60)[GFixes].events == 1, "only written rows counted");
        m.ingestLocated(kT0 + 11, 433.92e6, 54.7, 25.3, 120, {"a", "b", "c"});
        CHECK(m.timeline(100, 1u << GFixes).size() == 2, "a third site is a second row");
        m.ingestLocated(kT0, NAN, 1, 1, 1, {"a"});
        m.ingestLocated(kT0, 1e300, 1, 1, 1, {"a"});
    }

    return testSummary("eye");
}
