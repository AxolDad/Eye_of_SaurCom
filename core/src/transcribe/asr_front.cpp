// The speech engine's front end: see asr_front.h
#include "dect2/transcribe/asr_front.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace dect2 {
namespace {

// Butterworth section Q values, Q_k = 1 / (2 cos(pi (2k+1) / (2n))).
constexpr double kButterQ4[2] = {0.54119610014619698, 1.3065629648763765};                       // n = 4
constexpr double kButterQ6[3] = {0.51763809020504137, 0.70710678118654752, 1.9318516525781366};  // n = 6

// The same direct-form-1 biquad the class uses, for the free reading function below.
struct Bq {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    inline double step(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

long long gcdLL(long long a, long long b) {
    while (b) { const long long t = a % b; a = b; b = t; }
    return a < 0 ? -a : a;
}

// RBJ cookbook high pass, normalised so a0 = 1
void designHighPass(double fc, double fs, double q, double& b0, double& b1, double& b2, double& a1, double& a2) {
    const double w = 2 * math::kPi * fc / fs;
    const double cw = std::cos(w), sw = std::sin(w);
    const double alpha = sw / (2 * q);
    const double a0 = 1 + alpha;
    b0 = ((1 + cw) / 2) / a0;
    b1 = (-(1 + cw)) / a0;
    b2 = ((1 + cw) / 2) / a0;
    a1 = (-2 * cw) / a0;
    a2 = (1 - alpha) / a0;
}

void designLowPass(double fc, double fs, double q, double& b0, double& b1, double& b2, double& a1, double& a2) {
    const double w = 2 * math::kPi * fc / fs;
    const double cw = std::cos(w), sw = std::sin(w);
    const double alpha = sw / (2 * q);
    const double a0 = 1 + alpha;
    b0 = ((1 - cw) / 2) / a0;
    b1 = (1 - cw) / a0;
    b2 = ((1 - cw) / 2) / a0;
    a1 = (-2 * cw) / a0;
    a2 = (1 - alpha) / a0;
}

}  // namespace

// ---------------------------------------------------------------- Goertzel

void AsrFront::Goertzel::set(double hz, double fs) { coeff = 2 * std::cos(2 * math::kPi * hz / fs); }

// Returns the energy the block carries at this tone, comparable with sum(x*x): for a sinusoid of amplitude A over n samples the Goertzel
// magnitude is A n / 2, and such a sinusoid has energy n A^2 / 2, so energy = 2 |S|^2 / n.
double AsrFront::Goertzel::energy(const float* x, size_t n) const {
    if (n == 0) return 0;
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        const double s = coeff * s1 - s2 + (double)x[i];
        s2 = s1;
        s1 = s;
    }
    const double mag2 = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return mag2 > 0 ? 2 * mag2 / (double)n : 0;
}

// ---------------------------------------------------------------- setup

AsrFront::AsrFront() = default;

size_t AsrFront::frameLenFor(int sampleRate) {
    if (sampleRate <= 0) return 0;
    // Both tones are multiples of 600 Hz, so a frame length that is a whole number of 600 Hz periods puts each of them exactly on a bin
    // centre. The smallest such length is rate / gcd(rate, 600); take the multiple of it nearest 15 ms.
    const long long g = (long long)sampleRate / gcdLL(sampleRate, 600);
    const long long want = (long long)std::llround(sampleRate * 0.015);
    const long long k = std::max(1ll, (want + g / 2) / g);
    return (size_t)(k * g);
}

void AsrFront::prepare(int sampleRate) {
    rate_ = sampleRate;
    frameLen_ = frameLenFor(sampleRate);
    acc_.clear();
    acc_.reserve(frameLen_);
    fadeLen_ = std::max(1, (int)std::lround(sampleRate * (double)o_.fadeMs / 1000.0));
    holdLeft_ = 0;
    gain_ = 1;
    design();
}

void AsrFront::design() {
    if (rate_ <= 0) { bandOk_ = false; return; }
    // Leave the band pass out when the rate cannot carry it: the low-pass corner must sit well below Nyquist for the filter to mean anything.
    bandOk_ = o_.lowPassHz > o_.highPassHz && (double)rate_ > 2.5 * (double)o_.lowPassHz;
    if (bandOk_) {
        for (int i = 0; i < 3; i++)
            designHighPass(o_.highPassHz, rate_, kButterQ6[i], hp_[i].b0, hp_[i].b1, hp_[i].b2, hp_[i].a1, hp_[i].a2);
        for (int i = 0; i < 2; i++)
            designLowPass(o_.lowPassHz, rate_, kButterQ4[i], lp_[i].b0, lp_[i].b1, lp_[i].b2, lp_[i].a1, lp_[i].a2);
    }
    gA_.set(o_.burstToneAHz, rate_);
    gB_.set(o_.burstToneBHz, rate_);
    for (int i = 0; i < 3; i++) hp_[i].clear();
    for (int i = 0; i < 2; i++) lp_[i].clear();
}

void AsrFront::setOptions(const AsrFrontOptions& o) {
    const bool moved = o.highPassHz != o_.highPassHz || o.lowPassHz != o_.lowPassHz || o.burstToneAHz != o_.burstToneAHz ||
                       o.burstToneBHz != o_.burstToneBHz;
    const bool fade = o.fadeMs != o_.fadeMs;
    o_ = o;
    if (rate_ > 0) {
        if (moved) design();
        if (fade) fadeLen_ = std::max(1, (int)std::lround(rate_ * (double)o_.fadeMs / 1000.0));
    }
}

void AsrFront::clearStats() { st_ = AsrFrontStats(); }

void AsrFront::reset() {
    for (int i = 0; i < 3; i++) hp_[i].clear();
    for (int i = 0; i < 2; i++) lp_[i].clear();
    acc_.clear();
    holdLeft_ = 0;
    burstRun_ = 0;
    gain_ = 1;
}

// ---------------------------------------------------------------- processing

// One whole frame's worth of band-limited audio, measured. Speech spreads its energy over formants that move; a signalling burst puts nearly
// all of it on two fixed tones, so the share at those tones separates the two without a model.
void AsrFront::judge(const float* x, size_t n) {
    st_.frames++;
    double total = 0;
    for (size_t i = 0; i < n; i++) total += (double)x[i] * (double)x[i];
    // An all-but-silent frame is not a burst: there is nothing to be concentrated.
    const double tone = total > 1e-12 ? gA_.energy(x, n) + gB_.energy(x, n) : 0;
    const float ratio = total > 1e-12 ? (float)std::min(1.0, tone / total) : 0.f;
    st_.lastToneRatio = ratio;
    if (ratio >= o_.burstRatio) {
        // Only act once it has persisted: one tonal frame is a vowel, several in a row is signalling.
        if (++burstRun_ >= std::max(1, o_.burstFramesNeeded)) {
            // Mute the frame that follows plus the hold, so the burst's body and its tail both go.
            holdLeft_ = std::max(holdLeft_, (int)n + (int)std::lround(rate_ * (double)o_.burstHoldMs / 1000.0));
            st_.burstFrames++;
        }
    } else {
        burstRun_ = 0;
    }
}

void AsrFront::process(float* x, size_t n) {
    if (!o_.enabled || rate_ <= 0 || n == 0) return;
    const bool band = o_.bandpass && bandOk_;
    if (!band && !o_.blankBursts) return;   // nothing switched on: leave the samples alone, bit for bit
    st_.samples += n;

    if (band) {
        // Measure what is about to be thrown away, so the UI can say how much sub-audible tone the channel carries, then band-limit.
        double sub = 0;
        for (size_t i = 0; i < n; i++) {
            double v = (double)x[i];
            for (int s = 0; s < 3; s++) v = hp_[s].step(v);
            const double removed = (double)x[i] - v;   // what the high pass took out: the sub-audible part
            sub += removed * removed;
            for (int s = 0; s < 2; s++) v = lp_[s].step(v);
            x[i] = (float)v;
        }
        if (sub > 0) {
            const float db = (float)(10.0 * std::log10(sub / (double)n + 1e-20));
            st_.subAudibleDb = std::max(st_.subAudibleDb, db);
        }
    }

    if (!o_.blankBursts || frameLen_ == 0) return;

    // Detection measures a copy and acts on what comes next; the caller always gets back as many samples as it gave.
    const float step = 1.f / (float)fadeLen_;
    for (size_t i = 0; i < n; i++) {
        acc_.push_back(x[i]);
        if (acc_.size() >= frameLen_) {
            judge(acc_.data(), acc_.size());
            acc_.clear();
        }
        const float want = holdLeft_ > 0 ? 0.f : 1.f;
        if (gain_ < want) gain_ = std::min(want, gain_ + step);
        else if (gain_ > want) gain_ = std::max(want, gain_ - step);
        x[i] *= gain_;
        if (holdLeft_ > 0) holdLeft_--;
    }
}

// ---------------------------------------------------------------- the reading

float subAudibleLevelDb(const float* x, size_t n, int sampleRate, float cornerHz) {
    if (n == 0 || sampleRate <= 0 || cornerHz <= 0) return -120;
    Bq hp[3];
    for (int i = 0; i < 3; i++)
        designHighPass(cornerHz, sampleRate, kButterQ6[i], hp[i].b0, hp[i].b1, hp[i].b2, hp[i].a1, hp[i].a2);
    double sub = 0;
    for (size_t i = 0; i < n; i++) {
        double v = (double)x[i];
        for (int s = 0; s < 3; s++) v = hp[s].step(v);
        const double removed = (double)x[i] - v;
        sub += removed * removed;
    }
    return (float)(10.0 * std::log10(sub / (double)n + 1e-20));
}

}  // namespace dect2
