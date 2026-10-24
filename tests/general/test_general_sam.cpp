// Synchronous AM and the co-channel canceller (core/src/general/sam.cpp) on built signals: a carrier the loop must find and hold, the
// two sidebands told apart, and two AM stations on one channel where the weak one is only readable once the strong one is subtracted.
#include "dect2/general/sam.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
using namespace dect2::sam;
#include "../common/test_util.h"

static constexpr double kRate = 48000.0;
static const double kPi = dect2::math::kPi;

// An AM station: a carrier at offsetHz with amplitude amp, modulated by a tone at toneHz to a depth of m.
struct Station {
    double offsetHz = 0, amp = 1, toneHz = 1000, depth = 0.5, phase = 0, tonePhase = 0;
    cf32 at(double t) const {
        const double env = amp * (1.0 + depth * std::cos(2 * kPi * toneHz * t + tonePhase));
        const double ph = 2 * kPi * offsetHz * t + phase;
        return cf32((float)(env * std::cos(ph)), (float)(env * std::sin(ph)));
    }
};

// The level of a tone in a block of audio, against the whole block's power: how much of what is heard is that tone (dB).
static double toneDb(const std::vector<float>& a, double toneHz, size_t skip) {
    double re = 0, im = 0, pow = 0;
    size_t n = 0;
    for (size_t i = skip; i < a.size(); i++, n++) {
        const double t = (double)i / kRate;
        re += a[i] * std::cos(2 * kPi * toneHz * t);
        im += a[i] * std::sin(2 * kPi * toneHz * t);
        pow += (double)a[i] * a[i];
    }
    if (!n || pow <= 0) return -200;
    const double tone = 2.0 * (re * re + im * im) / ((double)n * (double)n);
    return 10 * std::log10(std::max(tone, 1e-20) / (pow / (double)n));
}

static double rms(const std::vector<float>& a, size_t skip) {
    double p = 0;
    size_t n = 0;
    for (size_t i = skip; i < a.size(); i++, n++) p += (double)a[i] * a[i];
    return n ? std::sqrt(p / (double)n) : 0;
}

int main() {
    const size_t N = (size_t)(kRate * 2.0);          // two seconds
    const size_t skip = (size_t)(kRate * 0.7);       // let the loop and the smoothers settle
    // the loop finds a carrier that is off the channel centre and holds it
    {
        for (double off : {0.0, 12.0, -37.5, 120.0}) {
            Station s;
            s.offsetHz = off; s.depth = 0.6; s.toneHz = 800;
            CarrierPll pll;
            pll.configure(kRate, 10.0, 200.0);
            for (size_t i = 0; i < N; i++) pll.push(s.at((double)i / kRate));
            CHECK(pll.locked() && pll.lock() > 0.95, "lock at %.1f Hz: %.3f", off, pll.lock());
            CHECK(std::fabs(pll.offsetHz() - off) < 0.5, "offset at %.1f Hz: found %.2f", off, pll.offsetHz());
            CHECK(std::fabs(pll.amplitude() - 1.0) < 0.05, "carrier amplitude at %.1f Hz: %.3f", off, pll.amplitude());
        }
        // noise alone: nothing to lock to
        CarrierPll pll;
        pll.configure(kRate, 10.0, 200.0);
        unsigned seed = 1;
        for (size_t i = 0; i < N; i++) {
            auto r = [&] { seed = seed * 1664525u + 1013904223u; return (double)(int)(seed >> 8) / 8388608.0 - 1.0; };
            pll.push(cf32((float)r(), (float)r()));
        }
        CHECK(!pll.locked(), "noise alone must not lock: %.3f", pll.lock());
    }
    // the phase shifter splits the sidebands: a line above the carrier is gone from the lower sideband and the other way round
    {
        for (int sign : {+1, -1}) {
            Hilbert90 h;
            h.configure(31);
            std::vector<float> keep, drop;
            for (size_t i = 0; i < N; i++) {
                const double t = (double)i / kRate, w = 2 * kPi * 2000.0 * sign * t;
                double iDel = 0, qSh = 0;
                h.push(cf32((float)std::cos(w), (float)std::sin(w)), iDel, qSh);
                keep.push_back((float)(sign > 0 ? iDel - qSh : iDel + qSh));   // the side the line is on
                drop.push_back((float)(sign > 0 ? iDel + qSh : iDel - qSh));   // the other side
            }
            const double k = rms(keep, skip), d = rms(drop, skip);
            CHECK(k > 0.9 && d < 0.05, "sign %+d: kept %.3f, dropped %.4f", sign, k, d);
            CHECK(20 * std::log10(k / std::max(d, 1e-9)) > 30, "sideband rejection %.1f dB", 20 * std::log10(k / std::max(d, 1e-9)));
        }
    }
    // synchronous AM: the audio comes through, and either sideband alone carries all of it
    {
        for (int side : {Dsb, Usb, Lsb}) {
            Station s;
            s.offsetHz = 20; s.depth = 0.5; s.toneHz = 1000;
            SamDemod d;
            d.configure(kRate, 10.0);
            d.setSide(side);
            std::vector<float> a;
            for (size_t i = 0; i < N; i++) a.push_back(d.push(s.at((double)i / kRate)));
            CHECK(d.pll().locked(), "%s: locked", sideName(side));
            CHECK(toneDb(a, 1000.0, skip) > -1.0, "%s: the tone is nearly all of the audio (%.1f dB)", sideName(side), toneDb(a, 1000.0, skip));
            // depth 0.5 on a carrier of 1: the recovered modulation is about 0.5 peak, so an rms near 0.35
            CHECK(std::fabs(rms(a, skip) - 0.354) < 0.05, "%s: level %.3f", sideName(side), rms(a, skip));
        }
        // a station with no modulation gives silence, not a hum
        Station q;
        q.depth = 0; q.offsetHz = 5;
        SamDemod d;
        d.configure(kRate, 10.0);
        std::vector<float> a;
        for (size_t i = 0; i < N; i++) a.push_back(d.push(q.at((double)i / kRate)));
        CHECK(rms(a, skip) < 0.01, "unmodulated carrier: %.4f", rms(a, skip));
    }
    // picking a sideband steps around an interferer that sits on the other one
    {
        Station want, bad;
        want.offsetHz = 0; want.depth = 0.5; want.toneHz = 1000;
        bad.offsetHz = 2500; bad.amp = 0.8; bad.depth = 0.5; bad.toneHz = 400;   // a station above, inside the filter
        SamDemod lower, upper;
        lower.configure(kRate, 10.0); lower.setSide(Lsb);
        upper.configure(kRate, 10.0); upper.setSide(Usb);
        std::vector<float> lo, up;
        for (size_t i = 0; i < N; i++) {
            const double t = (double)i / kRate;
            const cf32 z = want.at(t) + bad.at(t);
            lo.push_back(lower.push(z));
            up.push_back(upper.push(z));
        }
        // the interferer beats against the wanted carrier at 2500 Hz; the lower sideband should be much cleaner of it
        const double loBad = toneDb(lo, 2500.0, skip), upBad = toneDb(up, 2500.0, skip);
        CHECK(loBad < upBad - 15, "the interferer is %.1f dB in the lower sideband, %.1f dB in the upper", loBad, upBad);
        CHECK(toneDb(lo, 1000.0, skip) > -3.0, "the wanted tone survives in the lower sideband: %.1f dB", toneDb(lo, 1000.0, skip));
    }
    // the co-channel canceller: a weak station under a strong one, 20 dB down and 40 Hz away
    {
        Station strong, weak;
        strong.offsetHz = 0; strong.amp = 1.0; strong.depth = 0.6; strong.toneHz = 700;
        weak.offsetHz = 40; weak.amp = 0.1; weak.depth = 0.6; weak.toneHz = 1900; weak.phase = 0.9;
        auto run = [&](bool cancel, double& strongDb, double& weakDb, double& nullDb) {
            CoChannel cc;
            cc.configure(kRate, 10.0);
            cc.setCleanSide(Lsb);          // the weak station sits above, so the strong one's lower sideband is the clean one
            SamDemod d;
            d.configure(kRate, 10.0);
            d.setSide(Dsb);
            std::vector<float> a;
            for (size_t i = 0; i < N; i++) {
                const double t = (double)i / kRate;
                cf32 z = strong.at(t) + weak.at(t);
                if (cancel) z = cc.push(z);
                a.push_back(d.push(z));
            }
            strongDb = toneDb(a, 700.0, skip);
            weakDb = toneDb(a, 1900.0, skip);
            nullDb = cancel ? cc.nullDepthDb() : 0;
            return cc.locked();
        };
        double s0 = 0, w0 = 0, n0 = 0, s1 = 0, w1 = 0, n1 = 0;
        run(false, s0, w0, n0);
        const bool locked = run(true, s1, w1, n1);
        printf("  plain: strong %.1f dB, weak %.1f dB | cancelled: strong %.1f dB, weak %.1f dB, null %.1f dB\n", s0, w0, s1, w1, n1);
        CHECK(locked, "the canceller locks the strong carrier");
        CHECK(w0 < -20, "without it the weak station is buried: %.1f dB", w0);
        CHECK(n1 > 18, "the strong station is pushed down %.1f dB", n1);
        CHECK(w1 > w0 + 60, "the weak station comes up by %.1f dB (from %.1f to %.1f)", w1 - w0, w0, w1);
        CHECK(w1 > s1 + 15, "after cancelling the weak station is what is heard: weak %.1f dB, strong left %.1f dB", w1, s1);
        CHECK(s1 < s0 - 20, "the strong station drops from %.1f to %.1f dB", s0, s1);
    }
    // depth 0 passes the channel through untouched, so the control can be turned down to compare
    {
        Station s;
        s.offsetHz = 10; s.depth = 0.5;
        CoChannel cc;
        cc.configure(kRate, 10.0);
        cc.setDepth(0.f);
        double worst = 0;
        std::vector<cf32> in;
        for (size_t i = 0; i < 2000; i++) in.push_back(s.at((double)i / kRate));
        const size_t d = (size_t)cc.delaySamples();
        for (size_t i = 0; i < in.size(); i++) {
            const cf32 out = cc.push(in[i]);
            if (i >= d) worst = std::max(worst, (double)std::abs(out - in[i - d]));
        }
        CHECK(d == 31, "the delay is the phase shifter's: %zu samples", d);
        CHECK(worst < 1e-6, "depth 0 passes the input through: worst difference %.2g", worst);
    }
    printf(fails ? "FAILED\n" : "general_sam ok\n");
    return fails ? 1 : 0;
}
