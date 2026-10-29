// P25 Phase 2 H-DQPSK modem (see p25p2.h): the receiver's demodulator and the test signal's modulator.
#include "dect2/p25/p25p2.h"
#include "dect2/dsp/exact_resampler.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {
namespace p25p2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kAlpha = 0.2;          // root raised cosine roll-off
constexpr int kSps = 8;                 // demodulator samples per symbol (48 kHz)
constexpr double kWork = kSymRate * kSps;
constexpr int kSpan = 8;                // filter span in symbols

double rrc(double t) {   // t in symbols
    const double a = kAlpha;
    if (std::fabs(t) < 1e-9) return 1.0 - a + 4 * a / kPi;
    if (std::fabs(std::fabs(t) - 1.0 / (4 * a)) < 1e-9)
        return a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    const double num = std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a));
    const double den = kPi * t * (1 - (4 * a * t) * (4 * a * t));
    return num / den;
}

// dibit -> phase change (P25 CQPSK): 00 +45, 01 +135, 10 -45, 11 -135 degrees
double phaseOf(int d) { static const double p[4] = {kPi / 4, 3 * kPi / 4, -kPi / 4, -3 * kPi / 4}; return p[d & 3]; }
int dibitOf(double ang) {   // nearest of the four
    if (ang >= 0) return ang < kPi / 2 ? 0 : 1;
    return ang > -kPi / 2 ? 2 : 3;
}
} // namespace

std::vector<cf32> modulate(const std::vector<uint8_t>& dibits, double fs, double offsetHz, float amp) {
    const double sps = fs / kSymRate;
    std::vector<std::complex<double>> sym(dibits.size());
    double ph = 0;
    for (size_t k = 0; k < dibits.size(); k++) { ph += phaseOf(dibits[k]); sym[k] = std::polar(1.0, ph); }
    const size_t n = (size_t)std::llround((double)dibits.size() * sps);
    std::vector<cf32> out(n);
    double norm = 0;
    for (int k = -kSpan; k <= kSpan; k++) norm += rrc(k) * rrc(k);
    const double g = amp / std::sqrt(norm);
    double lo = 0;
    const double loStep = 2 * kPi * offsetHz / fs;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / sps;   // symbols
        const long k0 = (long)std::floor(t) - kSpan, k1 = (long)std::ceil(t) + kSpan;
        std::complex<double> acc = 0;
        for (long k = std::max(0L, k0); k <= k1 && k < (long)sym.size(); k++) acc += sym[(size_t)k] * rrc(t - (double)k);
        acc *= g * std::polar(1.0, lo);
        lo += loStep;
        if (lo > kPi) lo -= 2 * kPi;
        out[i] = cf32((float)acc.real(), (float)acc.imag());
    }
    return out;
}

struct Demod::Impl {
    double inRate = 0, off = 0;
    bool ok = false;
    ExactResampler rs;
    std::vector<cf32> mix, rsb;
    double phase = 0;
    float taps[2 * kSpan * kSps + 1];
    std::vector<std::complex<float>> hist = std::vector<std::complex<float>>(4 * kSps * kSpan + 64);
    size_t hpos = 0;
    // timing: the matched filter output of the last few symbols, sampled at every phase
    std::complex<float> y[64];
    uint64_t n = 0;                     // matched filter outputs so far
    uint64_t nextTake = 0;              // the output index of the next symbol
    double disp[3] = {0, 0, 0};         // modulus dispersion one sample early, on time, one late
    double pw[3] = {0, 0, 0};
    int nDisp = 0;
    bool acquired = false;
    double acqDisp[kSps] = {}, acqPw[kSps] = {};
    int acqN = 0;
    std::complex<float> prevSym{0, 0};
    std::complex<double> rot4{0, 0};    // running mean of d^4: the carrier offset per symbol
    double level = 0;

    Impl() {
        double s = 0;
        for (int i = 0; i <= 2 * kSpan * kSps; i++) { taps[i] = (float)rrc((double)(i - kSpan * kSps) / kSps); s += taps[i] * taps[i]; }
        for (float& t : taps) t /= (float)std::sqrt(s);
    }
    void clear() {
        rs.reset(); phase = 0;
        std::fill(hist.begin(), hist.end(), std::complex<float>(0, 0)); hpos = 0;
        for (auto& v : y) v = 0;
        n = 0; nextTake = 0; nDisp = 0; acquired = false; acqN = 0;
        for (int i = 0; i < 3; i++) disp[i] = pw[i] = 0;
        for (int i = 0; i < kSps; i++) acqDisp[i] = acqPw[i] = 0;
        prevSym = 0; rot4 = 0; level = 0;
    }
    std::complex<float> mf(std::complex<float> x) {
        const size_t L = 2 * kSpan * kSps + 1;
        hist[hpos] = x;
        std::complex<float> acc = 0;
        size_t idx = hpos;
        for (size_t k = 0; k < L; k++) { acc += hist[idx] * taps[k]; idx = idx ? idx - 1 : hist.size() - 1; }
        hpos = (hpos + 1) % hist.size();
        return acc;
    }
    void symbol(std::complex<float> s, std::vector<uint8_t>& out) {
        const std::complex<float> d = s * std::conj(prevSym);
        prevSym = s;
        const std::complex<double> dd(d.real(), d.imag());
        const double m = std::abs(dd);
        if (m <= 0) { out.push_back(0); return; }
        const std::complex<double> u = dd / m, u4 = u * u * u * u;
        rot4 = 0.98 * rot4 + 0.02 * u4;
        const double off = std::abs(rot4) > 0.05 ? std::arg(-rot4) / 4 : 0;   // d^4 of a +-45 / +-135 degree step is -1
        double a = std::arg(dd) - off;
        while (a > kPi) a -= 2 * kPi;
        while (a < -kPi) a += 2 * kPi;
        out.push_back((uint8_t)dibitOf(a));
    }
    void sample(std::complex<float> v, std::vector<uint8_t>& out) {
        y[n & 63] = v;
        level = 0.999 * level + 0.001 * std::norm(v);
        n++;
        if (!acquired) {   // look at every phase for a while: the one with the steadiest modulus is the symbol instant
            const int p = (int)((n - 1) % kSps);
            const double m2 = std::norm(v);
            acqPw[p] += m2; acqDisp[p] += m2 * m2;
            if (p == kSps - 1 && ++acqN >= 120) {
                int best = 0; double bv = 1e300;
                for (int i = 0; i < kSps; i++) {
                    const double mean = acqPw[i] / acqN, var = acqDisp[i] / acqN - mean * mean;
                    const double r = mean > 0 ? var / (mean * mean) : 1e300;
                    if (r < bv) { bv = r; best = i; }
                }
                acquired = true;
                nextTake = n + (uint64_t)best;   // the next time the output index has that phase
                while ((nextTake - 1) % kSps != (uint64_t)best) nextTake++;
            }
            return;
        }
        if (n - 1 == nextTake + 1) {   // one sample after the symbol instant: early, on time and late are all in
            const std::complex<float> e = y[(nextTake - 1) & 63], o = y[nextTake & 63], l = y[(nextTake + 1) & 63];
            const std::complex<float> v3[3] = {e, o, l};
            for (int i = 0; i < 3; i++) { const double m2 = std::norm(v3[i]); pw[i] += m2; disp[i] += m2 * m2; }
            symbol(o, out);
            int adj = 0;
            if (++nDisp >= 48) {
                double r[3];
                for (int i = 0; i < 3; i++) { const double mean = pw[i] / nDisp; r[i] = mean > 0 ? (disp[i] / nDisp - mean * mean) / (mean * mean) : 1e9; }
                if (r[0] < 0.85 * r[1] && r[0] <= r[2]) adj = -1;
                else if (r[2] < 0.85 * r[1] && r[2] < r[0]) adj = 1;
                nDisp = 0;
                for (int i = 0; i < 3; i++) pw[i] = disp[i] = 0;
            }
            nextTake = (uint64_t)((int64_t)nextTake + kSps + adj);
        }
    }
};

Demod::Demod() : p_(std::make_unique<Impl>()) {}
Demod::~Demod() = default;

bool Demod::configure(double inRate, double offsetHz) {
    p_->inRate = inRate; p_->off = offsetHz;
    p_->ok = inRate >= kWork && p_->rs.configure(inRate, kWork, 8);
    p_->clear();
    return p_->ok;
}
void Demod::reset() { p_->clear(); }
double Demod::cfoHz() const { return std::abs(p_->rot4) > 0.05 ? std::arg(-p_->rot4) / 4 * kSymRate / (2 * kPi) : 0; }
double Demod::levelDb() const { return 10 * std::log10(p_->level + 1e-20); }

void Demod::feed(const cf32* x, size_t n, std::vector<uint8_t>& dib) {
    Impl& m = *p_;
    if (!m.ok || !x || !n) return;
    const double step = -2 * kPi * m.off / m.inRate;
    m.mix.resize(n);
    for (size_t i = 0; i < n; i++) {
        m.mix[i] = x[i] * cf32((float)std::cos(m.phase), (float)std::sin(m.phase));
        m.phase += step;
        if (m.phase > kPi) m.phase -= 2 * kPi; else if (m.phase < -kPi) m.phase += 2 * kPi;
    }
    m.rsb.clear();
    m.rs.process(m.mix.data(), n, m.rsb);
    for (const cf32& v : m.rsb) m.sample(m.mf(std::complex<float>(v.real(), v.imag())), dib);
}

} // namespace p25p2
} // namespace dect2
