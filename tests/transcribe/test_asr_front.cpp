// The speech engine's front end (core/include/dect2/transcribe/asr_front.h): what it removes, what it must not remove, and that it is the
// identity when it is switched off.
#include "dect2/transcribe/asr_front.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <cstdio>
#include <numeric>
#include <utility>
#include <vector>

using namespace dect2;
#include "../common/test_util.h"

namespace {

std::vector<float> tone(double hz, double sec, int rate, float amp = 0.5f, double phase = 0) {
    std::vector<float> v((size_t)(sec * rate));
    for (size_t i = 0; i < v.size(); i++) v[i] = amp * (float)std::sin(2 * math::kPi * hz * (double)i / rate + phase);
    return v;
}

void addTone(std::vector<float>& v, double hz, int rate, float amp) {
    for (size_t i = 0; i < v.size(); i++) v[i] += amp * (float)std::sin(2 * math::kPi * hz * (double)i / rate);
}

double rms(const std::vector<float>& v, size_t from = 0) {
    if (v.size() <= from) return 0;
    double s = 0;
    for (size_t i = from; i < v.size(); i++) s += (double)v[i] * v[i];
    return std::sqrt(s / (double)(v.size() - from));
}

double db(double a, double b) { return 20 * std::log10((a + 1e-20) / (b + 1e-20)); }

// Something with the shape of speech: a moving formant pair, which is what the burst detector must not mistake for a signalling tone.
std::vector<float> speechLike(double sec, int rate) {
    std::vector<float> v((size_t)(sec * rate), 0.f);
    for (size_t i = 0; i < v.size(); i++) {
        const double t = (double)i / rate;
        const double f1 = 500 + 180 * std::sin(2 * math::kPi * 3.1 * t);     // formants that move, as a voice does
        const double f2 = 1700 + 450 * std::sin(2 * math::kPi * 2.3 * t);
        const double env = 0.5 + 0.5 * std::sin(2 * math::kPi * 4.0 * t);    // syllables
        v[i] = (float)(env * (0.30 * std::sin(2 * math::kPi * f1 * t) + 0.18 * std::sin(2 * math::kPi * f2 * t) +
                              0.05 * std::sin(2 * math::kPi * 2900 * t)));   // a little fricative energy up top
    }
    return v;
}

void run(AsrFront& f, std::vector<float>& v, size_t chunk) {
    for (size_t i = 0; i < v.size(); i += chunk) f.process(v.data() + i, std::min(chunk, v.size() - i));
}

}  // namespace

int main() {
    // ---- the analysis frame puts both signalling tones on a Goertzel bin centre, at every rate we see
    for (int rate : {8000, 11025, 16000, 22050, 44100, 48000}) {
        const size_t n = AsrFront::frameLenFor(rate);
        CHECK(n > 0, "frameLenFor(%d) is zero", rate);
        const double binsA = (double)n * 1200.0 / rate, binsB = (double)n * 1800.0 / rate;
        CHECK(std::fabs(binsA - std::lround(binsA)) < 1e-9, "1200 Hz is not on a bin at %d Hz (%.6f bins)", rate, binsA);
        CHECK(std::fabs(binsB - std::lround(binsB)) < 1e-9, "1800 Hz is not on a bin at %d Hz (%.6f bins)", rate, binsB);
        const double ms = 1000.0 * (double)n / rate;
        CHECK(ms > 7 && ms < 30, "the frame at %d Hz is %.1f ms, nowhere near 15", rate, ms);
    }

    const int rate = 16000;

    // ---- CTCSS is removed, and the speech band is not
    {
        AsrFrontOptions o;
        o.blankBursts = false;                   // this test is about the band pass alone
        AsrFront f;
        f.setOptions(o);
        f.prepare(rate);
        std::vector<float> v = tone(100, 0.5, rate, 0.5f);   // a CTCSS-like sub-audible tone
        const double before = rms(v);
        run(f, v, 256);
        const size_t settle = rate / 20;                      // skip the filter's first 50 ms
        const double after = rms(v, settle);
        CHECK(db(after, before) < -30, "100 Hz was only cut by %.1f dB", db(after, before));
        CHECK(f.stats().subAudibleDb > -30, "the sub-audible reading did not see the tone (%.1f dBFS)", f.stats().subAudibleDb);

        // The two CTCSS frequencies worth naming: the highest one there is, and a common one.
        for (auto tc : {std::pair<double, double>{254.1, -8.0}, std::pair<double, double>{203.5, -18.0}}) {
            AsrFront g;
            g.setOptions(o);
            g.prepare(rate);
            std::vector<float> s = tone(tc.first, 0.5, rate, 0.5f);
            const double sb = rms(s);
            run(g, s, 256);
            const double got = db(rms(s, settle), sb);
            CHECK(got < tc.second, "%.1f Hz was only cut by %.1f dB (wanted better than %.1f)", tc.first, got, tc.second);
        }

        AsrFront h;
        h.setOptions(o);
        h.prepare(rate);
        std::vector<float> k = tone(1000, 0.5, rate, 0.5f);   // mid-band speech: must come through
        const double kb = rms(k);
        run(h, k, 256);
        CHECK(std::fabs(db(rms(k, settle), kb)) < 1.5, "1000 Hz moved by %.1f dB", db(rms(k, settle), kb));
    }

    // ---- a 1200/1800 Hz signalling burst is muted
    {
        AsrFront f;
        f.prepare(rate);
        std::vector<float> v = tone(1200, 0.4, rate, 0.4f);
        addTone(v, 1800, rate, 0.4f);
        run(f, v, 256);
        CHECK(f.stats().burstFrames > 0, "no frame was judged a burst");
        CHECK(f.blanking(), "the blanker is not holding at the end of a burst");
        // The detector acts one frame late by design, so measure well past the start.
        const double tail = rms(v, (size_t)(0.1 * rate));
        CHECK(20 * std::log10(tail + 1e-20) < -40, "the burst was still at %.1f dBFS after 100 ms", 20 * std::log10(tail + 1e-20));
    }

    // ---- speech is not muted
    {
        AsrFront f;
        f.prepare(rate);
        std::vector<float> v = speechLike(0.6, rate);
        const double before = rms(v, (size_t)(0.05 * rate));
        run(f, v, 256);
        const double after = rms(v, (size_t)(0.05 * rate));
        CHECK(f.stats().burstFrames == 0, "%zu frames of speech were judged bursts", f.stats().burstFrames);
        CHECK(db(after, before) > -4, "speech lost %.1f dB through the chain", db(after, before));
    }

    // ---- speech riding on a CTCSS tone: the tone goes, the speech stays
    {
        AsrFront f;
        f.prepare(rate);
        std::vector<float> clean = speechLike(0.6, rate);
        std::vector<float> mixed = clean;
        addTone(mixed, 88.5, rate, 0.6f);        // a real CTCSS frequency, louder than the speech
        AsrFront g;
        g.prepare(rate);
        run(f, clean, 256);
        run(g, mixed, 256);
        const size_t settle = (size_t)(0.08 * rate);
        double num = 0, den = 0;
        for (size_t i = settle; i < clean.size(); i++) {
            const double d = (double)mixed[i] - (double)clean[i];
            num += d * d;
            den += (double)clean[i] * clean[i];
        }
        const double errDb = 10 * std::log10((num + 1e-20) / (den + 1e-20));
        CHECK(errDb < -20, "the tone left %.1f dB of error on the speech", errDb);
        CHECK(g.stats().burstFrames == 0, "the CTCSS tone was judged a signalling burst");
    }

    // ---- switched off, it is the identity, bit for bit
    {
        AsrFrontOptions o;
        o.enabled = false;
        AsrFront f;
        f.setOptions(o);
        f.prepare(rate);
        std::vector<float> v = speechLike(0.2, rate), was = v;
        run(f, v, 100);
        CHECK(v == was, "the chain changed the audio while switched off");

        AsrFrontOptions o2;
        o2.bandpass = false;
        o2.blankBursts = false;
        AsrFront g;
        g.setOptions(o2);
        g.prepare(rate);
        std::vector<float> w = speechLike(0.2, rate), wwas = w;
        run(g, w, 100);
        CHECK(w == wwas, "the chain changed the audio with every stage off");
    }

    // ---- the result does not depend on how the audio is chopped up
    {
        std::vector<float> src = speechLike(0.5, rate);
        addTone(src, 100, rate, 0.3f);
        std::vector<float> a = src, b = src, c = src;
        AsrFront fa, fb, fc;
        fa.prepare(rate); fb.prepare(rate); fc.prepare(rate);
        run(fa, a, a.size());     // all at once
        run(fb, b, 256);
        run(fc, c, 37);           // an awkward size that never lines up with the frame
        double worst = 0;
        for (size_t i = 0; i < src.size(); i++) {
            worst = std::max(worst, (double)std::fabs(a[i] - b[i]));
            worst = std::max(worst, (double)std::fabs(a[i] - c[i]));
        }
        CHECK(worst < 1e-5, "chunking changed the output by %.3g", worst);
    }

    // ---- a rate too low to hold the band leaves the band pass out rather than building a nonsense filter
    {
        AsrFront f;
        f.prepare(6000);          // 2.5 x 3400 Hz is above this
        std::vector<float> v = tone(100, 0.2, 6000, 0.5f), was = v;
        f.process(v.data(), v.size());
        CHECK(v == was, "the band pass ran at a rate that cannot carry it");
    }

    // ---- reset forgets the hold, so a new transmission does not start muted
    {
        AsrFront f;
        f.prepare(rate);
        std::vector<float> v = tone(1200, 0.2, rate, 0.4f);
        addTone(v, 1800, rate, 0.4f);
        run(f, v, 256);
        CHECK(f.blanking(), "expected to be blanking after a burst");
        f.reset();
        CHECK(!f.blanking(), "reset did not clear the blanker");
    }

    return testSummary("asr_front");
}
