// EventRing and events as JSON lines: round trip, rejects, truncation, overrun accounting, site-tagged events skipped.
#include "../common/test_util.h"
#include "dect2/net/event_ring.h"
#include <cmath>

using namespace dect2;
using automation::Event;

int main() {
    {   // round trip
        Event e("adsb.aircraft");
        e.at(1700000000.25, "ADS-B", 1090e6).num("alt", 35000).str("call", "BAW12\"A\n").flag("onGround", false);
        std::string line = eventToJson(e);
        CHECK(line.find('\n') == std::string::npos, "one line");
        Event o; std::string err;
        CHECK(eventFromJson(line, o, err), "parse: %s", err.c_str());
        CHECK(o.type == e.type && o.time == e.time && o.mode == e.mode && o.freqHz == e.freqHz, "head fields");
        CHECK(o.f.size() == 3, "3 fields, got %zu", o.f.size());
        CHECK(o.f["alt"].t == automation::Value::Num && o.f["alt"].n == 35000, "num");
        CHECK(o.f["call"].t == automation::Value::Str && o.f["call"].s == "BAW12\"A\n", "str");
        CHECK(o.f["onGround"].t == automation::Value::Bool && !o.f["onGround"].b, "bool");
    }
    {   // NaN / Inf have no JSON form: they go out as null and the event still parses (null time/freq read back as 0)
        Event e("t");
        e.at(NAN, "m", INFINITY).num("bad", NAN).num("inf", -INFINITY).num("ok", 2);
        std::string line = eventToJson(e);
        Event o; std::string err;
        CHECK(eventFromJson(line, o, err), "non-finite line parses: %s [%s]", err.c_str(), line.c_str());
        CHECK(o.type == "t" && o.time == 0 && o.freqHz == 0, "null time/freq -> 0");
        CHECK(o.f["bad"].t == automation::Value::Null && o.f["inf"].t == automation::Value::Null, "non-finite field -> Null");
        CHECK(o.f["ok"].t == automation::Value::Num && o.f["ok"].n == 2, "finite field kept");
    }
    {   // rejects
        Event o; std::string err;
        CHECK(!eventFromJson("{\"f\":{}}", o, err) && !err.empty(), "missing type");
        CHECK(!eventFromJson("{\"type\":\"x\",\"f\":{\"a\":{\"b\":1}}}", o, err), "nested object");
        CHECK(!eventFromJson("{\"type\":\"x\",\"f\":{\"a\":[1]}}", o, err), "nested array");
        CHECK(!eventFromJson("{\"type\":\"x\",\"f\":5}", o, err), "f not object");
        CHECK(!eventFromJson("not json", o, err), "garbage");
        CHECK(eventFromJson("{\"type\":\"x\"}", o, err) && o.f.empty(), "f optional");
        std::string big(5000, 'z');
        CHECK(eventFromJson("{\"type\":\"x\",\"f\":{\"s\":\"" + big + "\"}}", o, err), "long string parses");
        CHECK(o.f["s"].s.size() == automation::kMaxFieldText, "cut to %zu", o.f["s"].s.size());
    }
    {   // ring overrun
        EventRing r(4);
        for (int i = 0; i < 6; i++) { Event e("t"); e.num("i", i); r.push(e); }
        CHECK(r.head() == 6, "head %llu", (unsigned long long)r.head());
        uint64_t cur = 0, missed = 0; std::vector<std::string> lines;
        size_t n = r.since(cur, lines, 100, missed);
        CHECK(n == 4 && lines.size() == 4, "4 lines, got %zu", n);
        CHECK(missed == 2 && cur == 6, "missed %llu cursor %llu", (unsigned long long)missed, (unsigned long long)cur);
        Event o; std::string err;
        CHECK(eventFromJson(lines[0], o, err) && o.f["i"].n == 2, "oldest kept is 2");
        CHECK(eventFromJson(lines[3], o, err) && o.f["i"].n == 5, "newest is 5");
        lines.clear();
        CHECK(r.since(cur, lines, 100, missed) == 0 && lines.empty() && missed == 0, "second since empty");
        // max limit
        cur = 0; lines.clear();
        CHECK(r.since(cur, lines, 3, missed) == 3 && cur == 5 && missed == 2, "max 3: cursor %llu", (unsigned long long)cur);
        CHECK(r.since(cur, lines, 3, missed) == 1 && cur == 6 && missed == 0, "rest");
        // cursor from the future (ring restarted) resets
        cur = 99; lines.clear();
        r.since(cur, lines, 10, missed);
        CHECK(cur == 6, "future cursor clamped");
    }
    {   // site-tagged events are not re-published
        EventRing r(4);
        Event e("t"); e.str("site", "b");
        r.push(e);
        CHECK(r.head() == 0, "skipped");
        Event f("t"); r.push(f);
        CHECK(r.head() == 1, "plain pushed");
    }
    {
        CHECK(&globalEventRing() == &globalEventRing(), "singleton");
    }
    return testSummary("event_ring");
}
