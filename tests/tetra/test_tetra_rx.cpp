// TETRA receiver on the test signal: carriers found in the spectrum, synchronised, SYNC / SYSINFO / neighbour broadcasts decoded;
// +-2 kHz tuning error, +-100 ppm clock error (and a radio 100 ppm off in both), 15 dB SNR, two carriers in one capture, a low sample
// rate, and noise only.
#include "dect2/tetra/tetra_gen.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_tel.h"
#include "dect2/engine/modes.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static const TetraCell* find(const TetraTelemetry& t, int cc) {
    for (const auto& c : t.cells) if (c.colourCode == cc) return &c;
    return nullptr;
}

static TetraTelemetry run(int scene, double snrDb, double cfoHz, double ppm, double rate, int blocks) {
    SynthConfig sc;
    sc.mode = 48; sc.snrDb = snrDb; sc.cfoHz = cfoHz; sc.sroPpm = ppm; sc.modeOpt[0] = scene;
    auto syn = makeTetraSynth(sc, rate);
    TetraReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(kTetraTestCentreHz);
    const size_t chunk = (size_t)(rate * 0.05);
    std::vector<cf32> buf(chunk);
    uint64_t guard = 0;
    TetraTelemetry t;
    for (;;) {
        syn->generate(buf.data(), chunk);
        rx.feed(buf.data(), chunk);
        while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        rx.telemetry(t, 0);
        if (t.blocks >= (uint64_t)blocks || ++guard > 4000) break;
    }
    rx.feed(buf.data(), 1);
    rx.telemetry(t, 0);
    return t;
}

// the cell of scene 0 (or the first of scene 1), fully read
static void checkCellA(const TetraTelemetry& t, double cfo, const char* what) {
    const TetraCellCfg want = tetraTestCell(0, 0);
    const TetraCell* c = find(t, want.sync.colourCode);
    CHECK(c != nullptr, "%s: cell not found (%s)", what, tetraSummary(t).c_str());
    if (!c) return;
    CHECK(c->mcc == 1 && c->mnc == 1, "%s: MCC %d MNC %d", what, c->mcc, c->mnc);
    CHECK(c->hasSysinfo && c->la == 1001 && c->mainCarrier == 3696 && c->band == 3 && c->offset == 3, "%s: SYSINFO LA %d carrier %d", what, c->la, c->mainCarrier);
    CHECK(c->isMain && std::fabs(c->nominalHz - 392.4125e6) < 1 && std::fabs(c->uplinkHz - 382.4125e6) < 1, "%s: frequencies %.4f %.4f", what, c->nominalHz / 1e6, c->uplinkHz / 1e6);
    CHECK(std::fabs(c->cfoHz - cfo) < 300, "%s: offset %.0f Hz, want %.0f", what, c->cfoHz, cfo);
    CHECK(c->encryption() && c->aie && c->secClass3 && c->authRequired && c->encryptedPdus > 0, "%s: encryption", what);
    CHECK(c->hasNwrk && c->neighbours.size() == 2 && c->netTime.rfind("2026-10-12 09:30:00", 0) == 0, "%s: neighbours %zu, time '%s'", what, c->neighbours.size(), c->netTime.c_str());
    bool nb = false;
    for (const auto& n : c->neighbours) if (n.cellId == 1 && std::fabs(n.freqHz - 392.6e6) < 1 && n.la == 1002) nb = true;
    CHECK(nb, "%s: neighbour 1", what);
    CHECK(c->tsUsage[0] == 2 && c->tsUsage[1] >= 4 && c->tsUsage[2] == 0, "%s: timeslot use", what);
}

int main() {
    const ModeTuning mt = tetraTuning();
    CHECK(mt.stdMode == 48 && modeTuningById("tetra") && modeTuningById("tetra")->stdMode == 48 && modeTuning(48), "tuning table");
    struct Case { const char* name; double snr, cfo, ppm, rate; };
    const Case cases[] = {
        {"+2 kHz", 30, 2000, 0, 2e6},
        {"-2 kHz", 30, -2000, 0, 2e6},
        {"clock +100 ppm", 30, 0, 100, 2e6},
        {"clock -100 ppm", 30, 0, -100, 2e6},
        {"radio 100 ppm off (39 kHz, +100 ppm)", 30, 39250, 100, 2e6},
        {"15 dB", 15, 900, 40, 2e6},
    };
    for (const auto& k : cases) {
        const TetraTelemetry t = run(0, k.snr, k.cfo, k.ppm, k.rate, 2);
        const TetraCell* c = find(t, 5);
        printf("  %-38s %s; SNR %.1f dB, offset %+.0f Hz, blocks ok %llu bad %llu\n", k.name, tetraSummary(t).c_str(), c ? c->snrDb : -99.f, c ? c->cfoHz : 0.0,
               (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        checkCellA(t, k.cfo, k.name);
        CHECK(t.cells.size() == 1, "%s: %zu cells", k.name, t.cells.size());
    }
    // two carriers in one capture, then at a low sample rate
    for (double rate : {2e6, 300e3}) {
        const TetraTelemetry t = run(1, 20, 500, 20, rate, 2);
        printf("  two carriers at %.1f Msps: %s\n", rate / 1e6, tetraSummary(t).c_str());
        checkCellA(t, 500, rate > 1e6 ? "two carriers, A" : "300 ksps, A");
        const TetraCell* b = find(t, 12);
        CHECK(b && b->hasSysinfo && b->la == 1002 && std::fabs(b->nominalHz - 392.6e6) < 1 && !b->encryption() && b->secClass1 && b->encryptedPdus == 0,
              "second carrier: %s", b ? "fields" : "not found");
        if (b) CHECK(b->neighbours.size() == 1 && std::fabs(b->neighbours[0].freqHz - 392.4125e6) < 1, "second carrier's neighbour");
        CHECK(t.cells.size() == 2, "%zu cells", t.cells.size());
    }
    // noise only: nothing
    {
        TetraReceiver rx;
        rx.configure(2e6);
        rx.setCenterHz(kTetraTestCentreHz);
        std::vector<cf32> n(200000);
        uint32_t s = 1;
        for (int k = 0; k < 60; k++) {
            for (auto& v : n) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.2f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.2f); }
            rx.feed(n.data(), n.size());
            while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        TetraTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.blocks >= 2 && t.cells.empty() && t.blocksOk == 0, "%zu cells found in noise (%llu blocks)", t.cells.size(), (unsigned long long)t.blocks);
    }
    // no gap between captures: the samples that arrive while a capture is decoded make the next one (voice is heard whole)
    {
        const double rate = 300e3;
        SynthConfig sc; sc.mode = 48; sc.snrDb = 30; sc.modeOpt[0] = 0;
        auto syn = makeTetraSynth(sc, rate);
        const size_t want = (size_t)(rate * 2.3);
        std::vector<cf32> x(2 * want + 10);
        syn->generate(x.data(), x.size());
        TetraReceiver rx;
        rx.configure(rate);
        rx.setCenterHz(kTetraTestCentreHz);
        rx.feed(x.data(), x.size());   // two captures in one call, the second one arriving while the first is decoded
        while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        rx.feed(x.data(), 1);          // the decoder is free: the waiting capture goes
        while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        TetraTelemetry t;
        rx.feed(x.data(), 1);
        rx.telemetry(t, 0);
        const TetraCell* c = find(t, 5);
        CHECK(t.blocks == 2 && c && c->seen == 2, "back-to-back captures: %llu decoded, cell seen %llu times", (unsigned long long)t.blocks, c ? (unsigned long long)c->seen : 0ull);
    }
    printf(fails ? "tetra rx: %d FAILED\n" : "tetra rx: all passed\n", fails);
    return fails ? 1 : 0;
}
