// COSPAS-SARSAT receiver on the generated test signal: every beacon of a cycle must arrive with its exact hex ID, clean, in noise, with carrier offsets
// of a few kHz, a bit rate that is 0.8 % off, reversed polarity and at other sample rates. Noise alone must give nothing.
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/sarsat/sarsat_tel.h"
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static SarsatTelemetry run(const SynthConfig& cfg, double rate, double seconds, bool noiseOnly = false) {
    SarsatReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-sarsatTuning().tuneOffsetHz);
    auto syn = makeSarsatSynth(cfg, rate);
    std::vector<cf32> buf(32768);
    SarsatTelemetry t;
    uint64_t seq = 0;
    unsigned s = 777;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        if (noiseOnly) {
            for (auto& v : buf) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.3f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.3f); }
        } else syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        SarsatTelemetry n;
        if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    return t;
}

static int countFound(const SarsatTelemetry& t, const char* what) {
    int found = 0;
    for (const sarsat::Spec& w : sarsat::testBeacons()) {
        std::vector<uint8_t> bits = sarsat::encode(w);
        sarsat::Beacon ref;
        sarsat::decode(bits, ref);
        bool ok = false;
        for (const SarsatEntry& e : t.log)
            if (e.b.hexId == ref.hexId && e.b.hasPos == ref.hasPos && std::fabs(e.b.lat - ref.lat) < 1e-6 && std::fabs(e.b.lon - ref.lon) < 1e-6 && e.b.idText == ref.idText) { ok = true; break; }
        CHECK(ok, "%s: missing beacon %s", what, ref.hexId.c_str());
        found += ok;
    }
    std::set<std::string> known;
    for (const sarsat::Spec& w : sarsat::testBeacons()) { std::vector<uint8_t> b = sarsat::encode(w); sarsat::Beacon r; sarsat::decode(b, r); known.insert(r.hexId); }
    int stray = 0;
    for (const SarsatEntry& e : t.log) if (!known.count(e.b.hexId)) { stray++; printf("  stray %s corr %d t %.2f snr %.1f cfo %.0f long %d id %s\n", e.b.hexId.c_str(), e.b.corrected, e.timeSec, e.snrDb, e.cfoHz, (int)e.b.longMsg, e.b.idText.c_str()); }
    CHECK(stray == 0, "%s: %d messages that were not sent", what, stray);
    return found;
}

int main() {
    const double rate = 256000;
    const double seconds = 5 * 3.6 + 3;   // five bursts of at most 0.62 s, 3 s apart
    {
        SynthConfig cfg; cfg.snrDb = 40;
        countFound(run(cfg, rate, seconds), "clean");
    }
    {
        SynthConfig cfg; cfg.snrDb = 22; cfg.cfoHz = 4500;
        countFound(run(cfg, rate, seconds), "22 dB, +4.5 kHz");
    }
    {
        SynthConfig cfg; cfg.snrDb = 20; cfg.cfoHz = -7200;
        countFound(run(cfg, rate, seconds), "20 dB, -7.2 kHz");
    }
    {   // bit rate 0.8 % fast
        SynthConfig cfg; cfg.snrDb = 30; cfg.modeVal[0] = 0.8; cfg.cfoHz = 1200;
        countFound(run(cfg, rate, seconds), "bit rate +0.8 %");
    }
    {
        SynthConfig cfg; cfg.snrDb = 30; cfg.modeVal[0] = -0.8;
        countFound(run(cfg, rate, seconds), "bit rate -0.8 %");
    }
    {   // reversed phase polarity
        SynthConfig cfg; cfg.snrDb = 30; cfg.modeOpt[1] = 1;
        countFound(run(cfg, rate, seconds), "reversed polarity");
    }
    {
        SynthConfig cfg; cfg.snrDb = 25; cfg.cfoHz = 2000;
        countFound(run(cfg, 1024000, seconds), "1.024 Msps");
        countFound(run(cfg, 2400000, seconds), "2.4 Msps");
    }
    {   // edges of the search range and weak signals
        SynthConfig cfg; cfg.snrDb = 25; cfg.cfoHz = 11500;
        countFound(run(cfg, rate, seconds), "+11.5 kHz");
        cfg.cfoHz = -11500;
        countFound(run(cfg, rate, seconds), "-11.5 kHz");
        cfg.cfoHz = 40000;
        countFound(run(cfg, rate, seconds), "+40 kHz (wide transponder)");
        cfg.cfoHz = -40000;
        countFound(run(cfg, rate, seconds), "-40 kHz (wide transponder)");
        cfg.cfoHz = 300; cfg.snrDb = 14;
        countFound(run(cfg, rate, seconds * 2), "14 dB");
    }
    {
        SynthConfig cfg;
        const SarsatTelemetry t = run(cfg, rate, 20, true);
        CHECK(t.log.empty(), "noise gave %zu messages", t.log.size());
    }
    return testSummary("test_sarsat_rx");
}
