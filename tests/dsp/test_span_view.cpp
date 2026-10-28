// The Span bar's pure logic (span_view.h): typed spans, the rate a wider span needs, the FFT size that keeps a narrow view sharp,
// and the occupied width of a signal for "Fit to this signal".
#include "dect2/dsp/span_view.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }

static void parsing() {
    struct Ok { const char* s; double hz; };
    const Ok ok[] = {
        {"50k", 50e3}, {"1.2M", 1.2e6}, {"200 kHz", 200e3}, {"2.4 MHz", 2.4e6}, {"2.4MHz", 2.4e6}, {"  10K ", 10e3},
        {"500", 500}, {"500 Hz", 500}, {"750hz", 750}, {"1.5 khz", 1.5e3}, {"8m", 8e6}, {"0.5 MHz", 0.5e6}, {"1G", 1e9},
        {"2,4 MHz", 2.4e6},   // a decimal comma, as typed in much of Europe
        {"6 Msps", 6e6}, {"300 ksps", 300e3},
    };
    for (const Ok& t : ok) {
        double hz = -1;
        const bool r = parseSpanHz(t.s, hz);
        CHECK(r && near(hz, t.hz), "parse \"%s\" -> %d %.1f (want %.1f)", t.s, (int)r, hz, t.hz);
    }
    const char* bad[] = {"", "   ", "abc", "k", "MHz", "1.2.3M", "-5k", "0", "0 kHz", "5 kq", "5kk", "12 parsecs", "nan", "inf", "1e400", "5 k Hz x"};
    for (const char* s : bad) {
        double hz = 123;
        CHECK(!parseSpanHz(s, hz), "garbage \"%s\" accepted as %.1f", s, hz);
    }
}

static void formatting() {
    CHECK(formatSpan(2.4e6) == "2.4 MHz", "fmt 2.4 MHz -> %s", formatSpan(2.4e6).c_str());
    CHECK(formatSpan(50e3) == "50 kHz", "fmt 50 kHz -> %s", formatSpan(50e3).c_str());
    CHECK(formatSpan(800) == "800 Hz", "fmt 800 Hz -> %s", formatSpan(800).c_str());
    CHECK(formatSpan(12.5e3) == "12.5 kHz", "fmt 12.5 kHz -> %s", formatSpan(12.5e3).c_str());
    CHECK(formatSpan(10e6) == "10 MHz", "fmt 10 MHz -> %s", formatSpan(10e6).c_str());
    CHECK(formatSpan(1.536e6) == "1.54 MHz", "fmt 1.536 MHz -> %s", formatSpan(1.536e6).c_str());
    CHECK(formatResolution(292.97) == "293 Hz/bin", "res -> %s", formatResolution(292.97).c_str());
    CHECK(formatResolution(36.6) == "36.6 Hz/bin", "res -> %s", formatResolution(36.6).c_str());
    CHECK(formatResolution(1220.7) == "1.22 kHz/bin", "res -> %s", formatResolution(1220.7).c_str());
}

static void rates() {
    const std::vector<double> ladder = {0.25e6, 1e6, 2e6, 2.4e6, 4e6, 8e6, 10e6, 20e6};
    {   // the smallest allowed rate that holds the span
        RatePick p = rateForSpan(6e6, ladder, 0, 20e6);
        CHECK(p.ok && near(p.rateHz, 8e6) && !p.clamped, "6 MHz -> %.0f clamped %d", p.rateHz, (int)p.clamped);
        p = rateForSpan(2.4e6, ladder, 0, 20e6);
        CHECK(p.ok && near(p.rateHz, 2.4e6) && !p.clamped, "2.4 MHz -> %.0f", p.rateHz);
        p = rateForSpan(2.41e6, ladder, 0, 20e6);
        CHECK(p.ok && near(p.rateHz, 4e6), "2.41 MHz -> %.0f", p.rateHz);
    }
    {   // the mode's minimum: never below it, even for a narrow span
        RatePick p = rateForSpan(0.5e6, ladder, 2e6, 20e6);
        CHECK(p.ok && near(p.rateHz, 2e6), "min rate -> %.0f", p.rateHz);
    }
    {   // wider than the radio: its fastest, clamped
        RatePick p = rateForSpan(30e6, ladder, 0, 20e6);
        CHECK(p.ok && near(p.rateHz, 20e6) && p.clamped, "30 MHz -> %.0f clamped %d", p.rateHz, (int)p.clamped);
        p = rateForSpan(15e6, ladder, 0, 10e6);   // a radio whose maximum is below the list's top
        CHECK(p.ok && near(p.rateHz, 10e6) && p.clamped, "max 10 -> %.0f clamped %d", p.rateHz, (int)p.clamped);
    }
    {   // nothing to choose from: a file or a fixed-rate source
        RatePick p = rateForSpan(6e6, {}, 0, 0);
        CHECK(!p.ok, "empty list gave a rate");
        p = rateForSpan(6e6, ladder, 30e6, 20e6);   // the mode needs more than the radio gives
        CHECK(!p.ok, "impossible minimum gave a rate");
    }
    {   // unsorted list and a zero max (unknown) mean no cap from max
        RatePick p = rateForSpan(3e6, {10e6, 2e6, 3.2e6}, 0, 0);
        CHECK(p.ok && near(p.rateHz, 3.2e6), "unsorted -> %.0f", p.rateHz);
    }
}

static void fftSizes() {
    // at the full span the engine's own Auto size stays (8192..32768, bins of about 250 Hz or finer)
    CHECK(fftForSpan(2.4e6, 2.4e6) == 16384, "full 2.4 -> %d", fftForSpan(2.4e6, 2.4e6));
    CHECK(fftForSpan(20e6, 20e6) == 32768, "full 20 -> %d", fftForSpan(20e6, 20e6));
    CHECK(fftForSpan(0.25e6, 0.25e6) == 8192, "full 0.25 -> %d", fftForSpan(0.25e6, 0.25e6));
    // narrower: at least ~1000 bins across the view, a power of two, at most 65536
    CHECK(fftForSpan(240e3, 2.4e6) == 16384, "240k of 2.4M -> %d", fftForSpan(240e3, 2.4e6));
    CHECK(fftForSpan(100e3, 2.4e6) == 32768, "100k of 2.4M -> %d", fftForSpan(100e3, 2.4e6));
    CHECK(fftForSpan(50e3, 2.4e6) == 65536, "50k of 2.4M -> %d", fftForSpan(50e3, 2.4e6));
    CHECK(fftForSpan(1e3, 2.4e6) == 65536, "1k capped -> %d", fftForSpan(1e3, 2.4e6));
    CHECK(fftForSpan(0.5e6, 20e6) == 65536, "0.5M of 20M -> %d", fftForSpan(0.5e6, 20e6));
    CHECK(fftForSpan(2e6, 20e6) == 32768, "2M of 20M -> %d", fftForSpan(2e6, 20e6));
    for (double span : {1e3, 5e3, 20e3, 77e3, 300e3, 1e6, 2.4e6}) {
        const int n = fftForSpan(span, 2.4e6);
        CHECK((n & (n - 1)) == 0 && n >= 4096 && n <= 65536, "not a power of two in range: %d", n);
        const double across = n * span / 2.4e6;
        CHECK(across >= 1000 || n == 65536, "too few bins across %.0f: %.0f (n %d)", span, across, n);
    }
    CHECK(fftForSpan(0, 2.4e6) > 0 && fftForSpan(1e3, 0) > 0, "nonsense inputs give a usable size");
}

// a spectrum of n bins over fs: floor -100 dB with a little ripple, a carrier and two sidebands (an AM-like signal) at centre
static std::vector<float> amSpectrum(int n, int centre, int sideOff, int sideHalf) {
    std::vector<float> d(n);
    for (int i = 0; i < n; i++) d[i] = (float)(-100 + 1.5 * std::sin(i * 0.37) + 1.0 * std::sin(i * 1.91));
    d[centre] = -40;
    d[centre - 1] = d[centre + 1] = -60;
    for (int s = -1; s <= 1; s += 2)
        for (int k = -sideHalf; k <= sideHalf; k++) d[centre + s * sideOff + k] = std::max(d[centre + s * sideOff + k], (float)(-70 - std::abs(k) * 0.3));
    return d;
}

static void occupied() {
    {   // a flat-topped signal 41 bins wide: found exactly from a click a few bins off its peak
        std::vector<float> d(4096);
        for (int i = 0; i < 4096; i++) d[i] = (float)(-100 + 1.0 * std::sin(i * 0.7));
        for (int i = 2000; i <= 2040; i++) d[i] = -60;
        d[2020] = -55;
        OccupiedBins o = occupiedAround(d.data(), (int)d.size(), 2025, 8);
        CHECK(o.valid && o.lo == 2000 && o.hi == 2040 && o.peak == 2020, "flat: valid %d %d..%d peak %d", (int)o.valid, o.lo, o.hi, o.peak);
        CHECK(near(o.floorDb, -100, 0.03), "floor %.2f", o.floorDb);
    }
    {   // carrier with sidebands 30 bins away: the gap between them is bridged, so the sidebands count
        std::vector<float> d = amSpectrum(4096, 1000, 30, 6);
        OccupiedBins o = occupiedAround(d.data(), (int)d.size(), 1000, 4, 40);
        CHECK(o.valid && o.lo == 1000 - 36 && o.hi == 1000 + 36, "AM: %d..%d", o.lo, o.hi);
        // without the bridge only the carrier is found
        o = occupiedAround(d.data(), (int)d.size(), 1000, 4, 2);
        CHECK(o.valid && o.lo == 999 && o.hi == 1001, "carrier only: %d..%d", o.lo, o.hi);
    }
    {   // a click on the noise: nothing to fit
        std::vector<float> d(2048);
        for (int i = 0; i < 2048; i++) d[i] = (float)(-100 + 1.5 * std::sin(i * 0.37));
        OccupiedBins o = occupiedAround(d.data(), (int)d.size(), 700, 8);
        CHECK(!o.valid, "noise counted as a signal (%d..%d)", o.lo, o.hi);
    }
    {   // a signal at the edge of the spectrum stops at the edge
        std::vector<float> d(1024, -100.f);
        for (int i = 0; i < 12; i++) d[i] = -50;
        OccupiedBins o = occupiedAround(d.data(), (int)d.size(), 3, 8);
        CHECK(o.valid && o.lo == 0 && o.hi == 11, "edge: %d..%d", o.lo, o.hi);
    }
    {   // bad inputs
        std::vector<float> d(16, -100.f);
        CHECK(!occupiedAround(nullptr, 0, 0, 4).valid, "null");
        CHECK(!occupiedAround(d.data(), 16, -3, 4).valid && !occupiedAround(d.data(), 16, 99, 4).valid, "bin outside");
    }
    // the view for a fit: 1.5 x the width, at least 5 kHz
    CHECK(near(fitSpanHz(10e3), 15e3), "fit 10k -> %.0f", fitSpanHz(10e3));
    CHECK(near(fitSpanHz(1e3), 5e3), "fit 1k -> %.0f", fitSpanHz(1e3));
    CHECK(near(fitSpanHz(200e3, 5e3, 2.0), 400e3), "fit factor");
}

int main() {
    parsing();
    formatting();
    rates();
    fftSizes();
    occupied();
    printf(fails ? "span_view tests FAILED (%d)\n" : "span_view tests passed\n", fails);
    return fails ? 1 : 0;
}
