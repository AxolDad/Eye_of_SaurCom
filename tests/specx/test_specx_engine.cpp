// Spectrum explorer through the engine (setStandard(43)): the synthetic source plays a QPSK carrier and a tone in noise; the spectrum
// comes back through latestRx() and the region maths find the carrier's symbol rate.
#include "dect2/specx/specx_rx.h"
#include "dect2/specx/specx_gen.h"
#include "dect2/specx/specx_measure.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
int main() {
    const ModeTuning mt = specxTuning();
    CHECK(mt.stdMode == 43 && modeTuningById("specx") && modeTuningById("specx")->stdMode == 43, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = 2e6;
    t.synth.mode = 43;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(43);
    e.specx().setFftSize(4096);
    // the -3 dB width of an 8-frame average moves by several per cent from one snapshot to the next (the carrier check failed about one run
    // in six, fast machine or slow): 64 frames hold it well inside the 5 % the symbol rate is checked to
    e.specx().setAveraging(64);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 42, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    // at least 3 s, then until the receiver has made 20 reports and averaged 4 x 64 frames: a slow runner plays the test signal slower
    // than real time (the source waits for the receiver), so the signal heard in a fixed time varies
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 42) wrong++; }
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if ((el >= 3.0 && rx.specx.seq > 20 && rx.specx.frames >= 4 * 64) || el > 60) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const SpecxTelemetry& s = rx.specx;
    printf("  %d reports: %s\n", reports, specxSummary(s).c_str());
    // reports made by the receiver (its sequence number), not the polls that happened to see one
    CHECK(s.seq > 20 && wrong == 0, "%llu reports made (%d seen), %d wrong", (unsigned long long)s.seq, reports, wrong);
    CHECK(s.averaging == 64 && s.frames >= 4 * 64, "averaging %d, %llu frames", s.averaging, (unsigned long long)s.frames);
    CHECK(rx.rateOk && s.fftSize == 4096 && (int)s.spectrum.size() == 4096, "fft %d", s.fftSize);
    const double fs = s.rate, bin = fs / s.fftSize;
    CHECK(std::fabs(s.peakOffsetHz - kSpecxCwOffset * fs) < 2 * bin, "tone at %.0f", s.peakOffsetHz);
    specx::SpecView v; v.db = s.spectrum.data(); v.n = (int)s.spectrum.size(); v.binHz = bin; v.f0Hz = -fs / 2; v.enbw = s.enbw;
    const double rs = fs / kSpecxSymDiv, c = kSpecxQpskOffset * fs;
    const specx::Measure m = specx::measure(v, c - rs, c + rs);
    printf("  carrier: centre %.0f Hz, -3 dB %.0f Hz, -26 dB %.0f Hz, Rs %.0f (roll-off %.2f), SNR %.1f dB\n", m.centreHz, m.bw3Hz, m.bw26Hz, m.symRateBest, m.rollOff, m.snrDb);
    CHECK(m.flatTop && std::fabs(m.symRateBest - rs) < 0.05 * rs && std::fabs(m.centreHz - c) < 0.02 * rs, "carrier");
    // the nearest-table roll-off is a noisy snapshot: the measured -26/-3 dB ratio (about 1.33 for an ideal 0.35) can land under the 0.30 midpoint
    // and snap to 0.25, so check the ratio itself rather than the snapped class
    CHECK(m.bw3Hz > 0 && std::fabs(m.bw26Hz / m.bw3Hz - 1 - 0.35) < 0.1, "roll-off ratio %.3f (class %.2f)", m.bw26Hz / m.bw3Hz - 1, m.rollOff);
    const auto d = specx::detect(v);
    CHECK(d.size() == 2, "%zu signals", d.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "specx engine: %d FAILED\n" : "specx engine: all passed\n", fails);
    return fails ? 1 : 0;
}
