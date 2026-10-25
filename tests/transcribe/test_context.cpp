// The live context store (core/include/dect2/transcribe/context.h): which scopes a source draws from, how terms are ranked, and that the
// character budget is never overrun. Plus the ICAO-to-radio-name lookup the aviation feeder needs.
#include "dect2/transcribe/callsign.h"
#include "dect2/transcribe/context.h"
#include "dect2/transcribe/transcriber.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace dect2;
#include "../common/test_util.h"

namespace {

const int64_t kNow = 1760000000000ll;   // a fixed "now", so nothing here depends on the clock

ContextTerm term(const char* s, float w, int64_t ageSec) {
    ContextTerm t;
    t.spoken = s;
    t.weight = w;
    t.seenMs = kNow - (int64_t)(ageSec * 1000);
    return t;
}

SourceInfo aviation(double freq = 118700000, const char* chan = "118.700 Tower") {
    SourceInfo s;
    s.key = 1;
    s.freqHz = freq;
    s.mode = "airband";
    s.channel = chan;
    s.domain = "aviation";
    return s;
}

bool has(const std::vector<ContextTerm>& v, const std::string& s) {
    for (const auto& t : v) if (t.spoken == s) return true;
    return false;
}

size_t indexOf(const std::vector<ContextTerm>& v, const std::string& s) {
    for (size_t i = 0; i < v.size(); i++) if (v[i].spoken == s) return i;
    return (size_t)-1;
}

}  // namespace

int main() {
    // ---- the scopes a source draws from, widest first
    {
        const std::vector<std::string> sc = scopesFor(aviation());
        CHECK(sc.size() == 4, "expected 4 scopes, got %zu", sc.size());
        CHECK(sc[0] == "all", "the widest scope is %s", sc[0].c_str());
        CHECK(sc[1] == "aviation", "the domain scope is %s", sc[1].c_str());
        CHECK(sc[2] == scopeForChannel("118.700 Tower"), "the channel scope is %s", sc[2].c_str());
        CHECK(sc[3] == scopeForFreq(118700000), "the frequency scope is %s", sc[3].c_str());
        CHECK(scopeForFreq(118700000) == "freq:118700000", "frequency scope spelled %s", scopeForFreq(118700000).c_str());

        SourceInfo bare;             // no frequency, no channel: just the domain and the catch-all
        bare.domain = "marine";
        const std::vector<std::string> b = scopesFor(bare);
        CHECK(b.size() == 2 && b[1] == "marine", "a bare source drew %zu scopes", b.size());
    }

    // ---- terms come from every scope that applies, and not from one that does not
    {
        ContextStore c;
        c.setTerms("all", {term("Mayday", 1, 0)});
        c.setTerms("aviation", {term("Lufthansa 1492", 1, 0)});
        c.setTerms("marine", {term("Coast Guard", 1, 0)});
        c.setTerms(scopeForFreq(118700000), {term("runway two five left", 1, 0)});
        const auto v = c.termsFor(aviation(), 500, kNow);
        CHECK(has(v, "Mayday"), "the catch-all scope was not used");
        CHECK(has(v, "Lufthansa 1492"), "the domain scope was not used");
        CHECK(has(v, "runway two five left"), "the frequency scope was not used");
        CHECK(!has(v, "Coast Guard"), "a marine term leaked into an aviation source");
        CHECK(c.scopeCount() == 4 && c.termCount() == 4, "store holds %zu scopes / %zu terms", c.scopeCount(), c.termCount());
    }

    // ---- recent beats old, and heavy beats light at the same age
    {
        ContextStore c;
        c.setTerms("aviation", {term("old and heavy", 3.0f, 3600), term("new and light", 1.0f, 5)});
        const auto v = c.termsFor(aviation(), 500, kNow);
        CHECK(v.size() == 2, "expected 2 terms, got %zu", v.size());
        CHECK(v[0].spoken == "new and light", "ranking put '%s' first", v[0].spoken.c_str());

        ContextStore d;
        d.setTerms("aviation", {term("light", 1.0f, 10), term("heavy", 2.0f, 10)});
        const auto w = d.termsFor(aviation(), 500, kNow);
        CHECK(w[0].spoken == "heavy", "at equal age, ranking put '%s' first", w[0].spoken.c_str());
    }

    // ---- a term with no time is treated as old, not as brand new
    {
        ContextStore c;
        ContextTerm noTime;
        noTime.spoken = "no time";
        noTime.weight = 5;                       // much heavier, and still must lose to something recent
        c.setTerms("aviation", {noTime, term("recent", 1.0f, 2)});
        const auto v = c.termsFor(aviation(), 500, kNow);
        CHECK(v[0].spoken == "recent", "a term with no timestamp outranked a recent one ('%s' first)", v[0].spoken.c_str());
    }

    // ---- the age limit drops stale terms
    {
        ContextStore c;
        c.setTerms("aviation", {term("stale", 1, 4000), term("fresh", 1, 10)});
        const auto v = c.termsFor(aviation(), 500, kNow, 600 * 1000);
        CHECK(has(v, "fresh"), "the fresh term was dropped");
        CHECK(!has(v, "stale"), "the stale term survived the age limit");
    }

    // ---- the narrower scope wins a duplicate, and case does not make a second entry
    {
        ContextStore c;
        c.setTerms("aviation", {term("Speedbird 12", 1, 3000)});
        c.setTerms(scopeForFreq(118700000), {term("speedbird 12", 1, 1)});   // same name, fresher, from the narrower scope
        const auto v = c.termsFor(aviation(), 500, kNow);
        CHECK(v.size() == 1, "the duplicate was kept twice (%zu terms)", v.size());
        CHECK(v[0].seenMs == kNow - 1000, "the narrower scope's version did not win");
    }

    // ---- the character budget is never overrun, and a long name does not hide the shorter ones behind it
    {
        ContextStore c;
        std::vector<ContextTerm> t;
        t.push_back(term("aaaa", 1, 1));
        t.push_back(term(std::string(400, 'b').c_str(), 1, 1));   // far too long for the budget
        t.push_back(term("cccc", 1, 1));
        c.setTerms("aviation", t);
        const auto v = c.termsFor(aviation(), 40, kNow);
        size_t joined = 0;
        for (size_t i = 0; i < v.size(); i++) joined += v[i].spoken.size() + (i ? 2 : 0);
        CHECK(joined <= 40, "the joined terms are %zu characters, over a 40 budget", joined);
        CHECK(has(v, "aaaa") && has(v, "cccc"), "a term too long for the budget hid the ones after it");
    }

    // ---- the prompt sentence fits the budget it was given, lead-in included
    {
        ContextStore c;
        std::vector<ContextTerm> t;
        for (int i = 0; i < 60; i++) t.push_back(term(("Callsign " + std::to_string(1000 + i)).c_str(), 1, i));
        c.setTerms("aviation", t);
        for (size_t budget : {24u, 60u, 200u, 600u}) {
            const std::string p = c.promptFor(aviation(), budget, kNow);
            CHECK(p.size() <= budget, "a %zu-character budget produced %zu characters", budget, p.size());
            if (!p.empty()) {
                CHECK(p.rfind("Expect: ", 0) == 0, "the prompt does not start with the lead-in: %s", p.c_str());
                CHECK(p.back() == '.', "the prompt does not end in a full stop: %s", p.c_str());
            }
        }
        // The freshest names are the ones that survive a tight budget.
        const std::string tight = c.promptFor(aviation(), 60, kNow);
        CHECK(tight.find("Callsign 1000") != std::string::npos, "the freshest name was not kept: %s", tight.c_str());
    }

    // ---- nothing to say means an empty prompt, so the caller can leave its own alone
    {
        ContextStore c;
        CHECK(c.promptFor(aviation(), 600, kNow).empty(), "an empty store still produced a prompt");
        c.setTerms("aviation", {term("Lufthansa 1492", 1, 1)});
        CHECK(c.promptFor(aviation(), 4, kNow).empty(), "a budget too small for the lead-in still produced a prompt");
    }

    // ---- addTerm refreshes in place rather than piling up duplicates
    {
        ContextStore c;
        c.addTerm("aviation", term("Ryanair 34 Alpha", 1, 100));
        c.addTerm("aviation", term("ryanair 34 alpha", 2, 1));
        CHECK(c.termCount() == 1, "addTerm made %zu entries for one name", c.termCount());
        const auto v = c.termsFor(aviation(), 500, kNow);
        CHECK(v.size() == 1 && v[0].weight == 2, "the refreshed term did not replace the old one");
    }

    // ---- setTerms with an empty list removes the scope; clearScope and clear do what they say
    {
        ContextStore c;
        c.setTerms("aviation", {term("x", 1, 1)});
        c.setTerms("aviation", {});
        CHECK(c.scopeCount() == 0, "an empty list left %zu scopes", c.scopeCount());
        c.setTerms("all", {term("y", 1, 1)});
        c.setTerms("aviation", {term("z", 1, 1)});
        c.clearScope("all");
        CHECK(c.scopeCount() == 1 && c.scopes()[0] == "aviation", "clearScope removed the wrong thing");
        c.clear();
        CHECK(c.termCount() == 0, "clear left %zu terms", c.termCount());
    }

    // ---- the radio name of a designator, which is what makes the aviation feeder worth having
    {
        CHECK(icaoToTelephony("BAW") == "Speedbird", "BAW read as '%s'", icaoToTelephony("BAW").c_str());
        CHECK(icaoToTelephony("DLH") == "Lufthansa", "DLH read as '%s'", icaoToTelephony("DLH").c_str());
        CHECK(icaoToTelephony("ZZZ").empty(), "an unknown designator produced '%s'", icaoToTelephony("ZZZ").c_str());
        CHECK(icaoToTelephony("").empty(), "an empty designator produced something");
        CHECK(icaoToTelephony("BA").empty(), "a two-letter designator produced something");
        // Stable between calls: the UI and the prompt must not disagree about a name.
        CHECK(icaoToTelephony("BAW") == icaoToTelephony("BAW"), "the lookup is not stable");
        // And it round-trips with the table it came from.
        for (const char* code : {"BAW", "DLH", "RYR", "AAL", "UAL"}) {
            const std::string name = icaoToTelephony(code);
            CHECK(!name.empty(), "%s has no radio name", code);
            std::string lower = name;
            for (char& ch : lower) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            CHECK(telephonyToIcao(lower) == code, "'%s' did not round-trip back to %s (got %s)", name.c_str(), code,
                  telephonyToIcao(lower).c_str());
        }
    }

    // ---- spoken forms: what the engine should be told to expect
    {
        CHECK(spokenFlight("BAW472") == "Speedbird 472", "BAW472 said as '%s'", spokenFlight("BAW472").c_str());
        CHECK(spokenFlight("DLH123A") == "Lufthansa 123A", "DLH123A said as '%s'", spokenFlight("DLH123A").c_str());
        CHECK(spokenFlight("N739SP") == "N739SP", "a registration said as '%s'", spokenFlight("N739SP").c_str());
        CHECK(spokenFlight("XYZ123") == "XYZ 123", "an unknown designator said as '%s'", spokenFlight("XYZ123").c_str());
        CHECK(spokenFlight("baw472") == "Speedbird 472", "lower case said as '%s'", spokenFlight("baw472").c_str());
        CHECK(spokenFlight("BAW 472") == "Speedbird 472", "a spaced call sign said as '%s'", spokenFlight("BAW 472").c_str());
        CHECK(spokenFlight("").empty(), "an empty call sign produced something");
        CHECK(spokenFlight("AB") == "AB", "a two-character call sign said as '%s'", spokenFlight("AB").c_str());
        CHECK(spokenFlight("BAW") == "BAW", "a bare designator said as '%s'", spokenFlight("BAW").c_str());

        CHECK(spokenRunway("25L") == "runway two five left", "25L said as '%s'", spokenRunway("25L").c_str());
        CHECK(spokenRunway("09") == "runway zero niner", "09 said as '%s'", spokenRunway("09").c_str());
        CHECK(spokenRunway("3") == "runway three", "3 said as '%s'", spokenRunway("3").c_str());
        CHECK(spokenRunway("36R") == "runway three six right", "36R said as '%s'", spokenRunway("36R").c_str());
        CHECK(spokenRunway("17C") == "runway one seven center", "17C said as '%s'", spokenRunway("17C").c_str());
        CHECK(spokenRunway("").empty(), "an empty identifier produced something");
        CHECK(spokenRunway("37").empty(), "runway 37 does not exist, got '%s'", spokenRunway("37").c_str());
        CHECK(spokenRunway("00").empty(), "runway 00 does not exist, got '%s'", spokenRunway("00").c_str());
        CHECK(spokenRunway("25X").empty(), "a bad side letter produced '%s'", spokenRunway("25X").c_str());
        CHECK(spokenRunway("ABC").empty(), "letters produced '%s'", spokenRunway("ABC").c_str());
    }

    // ---- the global store is one store
    {
        ContextStore::global().clear();
        ContextStore::global().setTerms("aviation", {term("global term", 1, 1)});
        CHECK(ContextStore::global().termCount() == 1, "the global store did not keep the term");
        ContextStore::global().clear();
    }

    return testSummary("context");
}
