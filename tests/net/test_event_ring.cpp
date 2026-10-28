// EventRing and events as JSON lines: round trip, rejects, truncation, overrun accounting, site-tagged events skipped.
#include "../common/test_util.h"
#include "dect2/net/event_ring.h"
#include <cmath>

using namespace dect2;
using automation::Event;

int main() {
    {   // round trip
        Event e("adsb.aircraft");
        e.at(std::floor(automation::wallNow()) - 3.25, "ADS-B", 1090e6).num("alt", 35000).str("call", "BAW12\"A\n").flag("onGround", false);
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
    {   // untrusted input from another station: names, field count, time and frequency are bounded
        Event o; std::string err;
        const double now = automation::wallNow();
        const std::string longName(200, 'n');
        std::string line = "{\"type\":\"" + longName + "\",\"mode\":\"" + longName + "\",\"time\":" + std::to_string(now) + ",\"f\":{\"" + longName + "\":1";
        for (int i = 0; i < 100; i++) line += ",\"k" + std::to_string(1000 + i) + "\":" + std::to_string(i);
        line += "}}";
        CHECK(eventFromJson(line, o, err), "long names parse: %s", err.c_str());
        CHECK(o.type.size() == kMaxEventName && o.mode.size() == kMaxEventName, "type and mode cut to 64 (%zu, %zu)", o.type.size(), o.mode.size());
        CHECK(o.f.size() == kMaxEventFields, "at most 64 fields (%zu)", o.f.size());
        bool keysOk = true;
        for (const auto& kv : o.f) keysOk = keysOk && kv.first.size() <= kMaxEventName;
        CHECK(keysOk, "field names cut to 64");
        CHECK(std::fabs(o.time - now) < 1e-3, "a time near this clock is kept");
        // a UTF-8 name is not cut in the middle of a character: 63 ASCII bytes, then a two byte letter that would end at byte 65
        const std::string u = std::string(63, 'a') + "\xc3\xa9" + "zz";
        CHECK(eventFromJson("{\"type\":\"" + u + "\"}", o, err) && o.type == std::string(63, 'a'), "cut on a character boundary (%zu)", o.type.size());
        // time: more than a day off this clock in either direction is unknown (0); a non-finite one is refused
        CHECK(eventFromJson("{\"type\":\"x\",\"time\":" + std::to_string(now + 2 * 86400) + "}", o, err) && o.time == 0, "a far-future time becomes 0 (%f)", o.time);
        CHECK(eventFromJson("{\"type\":\"x\",\"time\":" + std::to_string(now - 2 * 86400) + "}", o, err) && o.time == 0, "a far-past time becomes 0");
        CHECK(eventFromJson("{\"type\":\"x\",\"time\":" + std::to_string(now + 3600) + "}", o, err) && std::fabs(o.time - now - 3600) < 1e-3, "an hour off is kept");
        // JSON has no Inf / NaN, and a number that overflows to one (1e999) is already refused by the parser: eventFromJson's own finite-time
        // check is the second line, for a parser that would let one through
        CHECK(!eventFromJson("{\"type\":\"x\",\"time\":1e999}", o, err), "an infinite time is refused");
        CHECK(!eventFromJson("{\"type\":\"x\",\"time\":-1e999}", o, err), "a negative infinite time is refused");
        CHECK(!eventFromJson("{\"type\":\"x\",\"time\":NaN}", o, err), "a NaN time is refused");
        // freq clamped to 0 .. 1e12
        CHECK(eventFromJson("{\"type\":\"x\",\"freq\":5e15}", o, err) && o.freqHz == kMaxEventFreqHz, "freq clamped to 1e12 (%g)", o.freqHz);
        CHECK(eventFromJson("{\"type\":\"x\",\"freq\":-5}", o, err) && o.freqHz == 0, "a negative freq becomes 0");
        CHECK(eventFromJson("{\"type\":\"x\",\"freq\":1e300}", o, err) && o.freqHz == kMaxEventFreqHz, "a huge freq is clamped (%g)", o.freqHz);
        CHECK(eventFromJson("{\"type\":\"x\",\"freq\":433.92e6}", o, err) && o.freqHz == 433.92e6, "a normal freq is kept");
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
