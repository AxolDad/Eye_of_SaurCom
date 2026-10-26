// Ionospheric sounders through the engine (setStandard(54)): the synthetic source plays the two-sounder test scene in real time at
// 1 Msps around 0.5 MHz; the 100 kHz/s sweep that starts at 2 s comes back through latestRx() as standard 53 with its ionogram.
#include "dect2/iono/iono_dsp.h"
#include "dect2/iono/iono_gen.h"
#include "dect2/iono/iono_rx.h"
#include "dect2/iono/iono_tel.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
int main() {
    const ModeTuning mt = ionoTuning();
    CHECK(mt.stdMode == 54 && modeTuningById("iono") && modeTuningById("iono")->stdMode == 54, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 0.5e6;
    t.bandwidthMhz = 1;
    t.sampleRate = 1e6;
    t.synth.mode = 54;
    t.synth.modeVal[0] = 0.5;
    FileOptions fo;
    e.setStandard(54);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 53, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    // until 4.5 s of signal (A's sweep found, B's at 7 s not yet): the source waits for a slow receiver and catches up after a stall, so
    // the signal heard in a fixed wall-clock time varies both ways
    const auto t0 = std::chrono::steady_clock::now();
    while (rx.iono.nowSec < 4.5 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 53) wrong++; }
        else std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const IonoTelemetry& s = rx.iono;
    printf("  %d reports: %s, %.2f s of signal\n", reports, ionoSummary(s).c_str(), s.nowSec);
    // reports made by the receiver (one each 0.25 s of signal, its sequence number), not the polls that happened to see one: the engine
    // passes on only the newest when it is starved, and a fast or slow runner polls a different share of them
    CHECK(s.seq > 8 && wrong == 0, "%llu reports made (%d seen), %d wrong", (unsigned long long)s.seq, reports, wrong);
    CHECK(rx.rateOk && s.state == 1 && s.covered.size() == (size_t)iono::kCols, "state %d", s.state);
    CHECK(s.sounders.size() == 1 && s.sounders[0].rateHz == 100e3, "%zu sounders", s.sounders.size());
    CHECK(s.sel == 0 && s.ionoDb.size() == (size_t)iono::kRows * iono::kCols, "ionogram %zu", s.ionoDb.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    // Clear (#244): no telemetry after it may name a sounder that is gone (the display indexed sounders[sel] and crashed). The sweep stays
    // in the span and is found again, so each report of the next second of signal is checked: its sel indexes its own sounders, and its
    // ionogram holds no more than the columns swept since the Clear (a report built before the Clear and published after it holds those
    // of before). The receiver's report right after clearResults() is the emptied one (or one built after it): the reference for both
    e.iono().clearResults();
    IonoTelemetry cleared;
    e.iono().telemetry(cleared, 0);
    auto sane = [&cleared](const IonoTelemetry& x, int& swept, int& sweptMax) {
        swept = 0;
        for (uint8_t c : x.swept) swept += c;
        sweptMax = x.sel >= 0 && x.sel < (int)x.sounders.size() ? (int)((x.nowSec - cleared.nowSec) * x.sounders[(size_t)x.sel].rateHz / iono::kColHz) + 3 : 0;
        return x.sel >= -1 && x.sel < (int)x.sounders.size() && x.ionoDb.empty() == (x.sel < 0) && (x.sounders.empty() || swept <= sweptMax);
    };
    int swept = 0, sweptMax = 0, after = 0, stale = 0;
    CHECK(sane(cleared, swept, sweptMax), "Clear: %zu sounders, sel %d, %d columns swept", cleared.sounders.size(), cleared.sel, swept);
    RxTelemetry a;
    const auto t1 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() < 20) {
        if (e.latestRx(a, seq)) {
            seq = a.seq;
            const IonoTelemetry& x = a.iono;
            if (x.nowSec <= cleared.nowSec) continue;   // made before the Clear, or the emptied report itself
            if (!sane(x, swept, sweptMax) && !stale++)
                printf("  after Clear: %zu sounders, sel %d, %d columns swept (%d at most in the %.2f s since)\n", x.sounders.size(), x.sel, swept, sweptMax,
                       x.nowSec - cleared.nowSec);
            after++;
            if (x.nowSec > cleared.nowSec + 1) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(after > 0 && stale == 0, "%d reports after Clear, %d with a stale sounder or sel", after, stale);
    e.stop();
    printf(fails ? "iono engine: %d FAILED\n" : "iono engine: all passed\n", fails);
    return fails ? 1 : 0;
}
