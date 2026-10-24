// Synchronous AM and the co-channel canceller, see sam.h
#include "dect2/general/sam.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace sam {

using dect2::math::kPi;

const char* sideName(int s) { return s == Usb ? "upper" : s == Lsb ? "lower" : "both"; }

// ---- carrier loop ----------------------------------------------------------------------------------------------------------------

void CarrierPll::configure(double rate, double loopBw, double pullIn) {
    rate_ = rate > 1 ? rate : 48000;
    // the usual second-order constants for a damping of 1/sqrt(2)
    const double zeta = 0.7071;
    auto gains = [&](double bw, double& al, double& be) {
        const double theta = std::clamp(bw, 0.1, rate_ / 8) / rate_, d = 1 + 2 * zeta * theta + theta * theta;
        al = 4 * zeta * theta / d;
        be = 4 * theta * theta / d;
    };
    gains(std::clamp(loopBw, 0.1, rate_ / 8), alpha_, beta_);
    gains(std::clamp(loopBw * 8, 0.1, rate_ / 8), wideAlpha_, wideBeta_);
    maxFreq_ = 2 * kPi * std::clamp(pullIn, 1.0, rate_ / 4) / rate_;
    ampK_ = 1 - std::exp(-2 * kPi * 2.0 / rate_);     // the carrier's amplitude, smoothed over about 80 ms
    lockK_ = 1 - std::exp(-2 * kPi * 5.0 / rate_);
    reset();
}

void CarrierPll::reset() { phase_ = freq_ = amp_ = lock_ = 0; }

cf32 CarrierPll::push(cf32 z) {
    const cf32 nco((float)std::cos(phase_), (float)-std::sin(phase_));
    const cf32 v = z * nco;
    const double re = v.real(), im = v.imag(), mag = std::sqrt(re * re + im * im);
    // the phase of the derotated sample: the right error while the carrier is present, which amplitude modulation keeps
    const double err = mag > 1e-12 ? std::atan2(im, re) : 0.0;
    amp_ += ampK_ * (re - amp_);                       // the audio averages out, so this settles on the carrier
    lock_ += lockK_ * ((mag > 1e-12 ? re / mag : 0.0) - lock_);   // the cosine of the error: 1 when the loop is on the carrier
    // hunting: the wide gains find a carrier that is well off the channel; once on it, the narrow ones ride through fading
    const bool hunting = lock_ < 0.8;
    const double al = hunting ? wideAlpha_ : alpha_, be = hunting ? wideBeta_ : beta_;
    freq_ = std::clamp(freq_ + be * err, -maxFreq_, maxFreq_);
    phase_ += freq_ + al * err;
    if (phase_ > kPi) phase_ -= 2 * kPi;
    else if (phase_ < -kPi) phase_ += 2 * kPi;
    return v;
}

// ---- 90 degree phase shift -------------------------------------------------------------------------------------------------------

void Hilbert90::configure(int halfTaps) {
    half_ = std::clamp(halfTaps, 3, 255);
    const int n = 2 * half_ + 1;
    h_.assign((size_t)n, 0.0);
    for (int k = -half_; k <= half_; k++) {
        if (k % 2 == 0) continue;                      // the even taps of an ideal shifter are zero
        const double w = 0.54 - 0.46 * std::cos(2 * kPi * (k + half_) / (n - 1));   // Hamming
        h_[(size_t)(k + half_)] = 2.0 / (kPi * k) * w;
    }
    bufI_.assign((size_t)n, 0.0);
    bufQ_.assign((size_t)n, 0.0);
    pos_ = 0;
}

void Hilbert90::reset() {
    std::fill(bufI_.begin(), bufI_.end(), 0.0);
    std::fill(bufQ_.begin(), bufQ_.end(), 0.0);
    pos_ = 0;
}

void Hilbert90::push(cf32 z, double& iDelayed, double& qShifted) {
    const size_t n = h_.size();
    if (n == 0) { iDelayed = z.real(); qShifted = 0; return; }
    bufI_[pos_] = z.real();
    bufQ_[pos_] = z.imag();
    pos_ = pos_ + 1 < n ? pos_ + 1 : 0;                // pos_ now points at the oldest sample
    double acc = 0;
    size_t j = pos_;
    for (size_t k = 0; k < n; k++) {                   // h is indexed oldest first, which is the convolution
        acc += h_[n - 1 - k] * bufQ_[j];
        j = j + 1 < n ? j + 1 : 0;
    }
    qShifted = acc;
    const size_t mid = (pos_ + (size_t)half_) % n;
    iDelayed = bufI_[mid];
}

// ---- synchronous AM --------------------------------------------------------------------------------------------------------------

void SamDemod::configure(double rate, double loopBw) {
    pll_.configure(rate, loopBw, 200.0);
    hil_.configure(31);
    dcK_ = 1 - std::exp(-2 * kPi * 20.0 / (rate > 1 ? rate : 48000));   // the carrier sits at 0 Hz; AM audio starts above it
    reset();
}

void SamDemod::setSide(int s) { side_ = s >= Dsb && s <= Lsb ? s : Dsb; }

void SamDemod::reset() {
    pll_.reset();
    hil_.reset();
    dc_ = 0;
}

float SamDemod::push(cf32 z) {
    const cf32 v = pll_.push(z);
    double iDel = 0, qSh = 0;
    hil_.push(v, iDel, qSh);
    // Upper = I - H{Q}, lower = I + H{Q}: an interferer above the carrier drops out of the lower sideband and the other way round.
    // A station of its own is symmetric about its carrier, so either sideband carries all of its audio at full level.
    const double x = side_ == Usb ? iDel - qSh : side_ == Lsb ? iDel + qSh : iDel;
    dc_ += dcK_ * (x - dc_);
    const double amp = std::max(pll_.amplitude(), 1e-9);
    return (float)((x - dc_) / amp);                   // the modulation itself, about -1 to 1, as the envelope detector gives
}

// ---- co-channel canceller --------------------------------------------------------------------------------------------------------

void CoChannel::configure(double rate, double loopBw) {
    rate_ = rate > 1 ? rate : 48000;
    pll_.configure(rate_, loopBw, 200.0);
    hil_.configure(31);
    mK_ = 1 - std::exp(-2 * kPi * 20.0 / rate_);
    powK_ = 1 - std::exp(-2 * kPi * 5.0 / rate_);
    mu_ = 0.002;                                  // slow: the residual holds the weak station, and a fast loop would chase that instead
    delay_.assign((size_t)std::max(1, hil_.delaySamples()), cf32(0, 0));
    phasor_.assign(delay_.size(), cf32(1, 0));
    reset();
}

void CoChannel::setCleanSide(int s) { clean_ = s >= Dsb && s <= Lsb ? s : Usb; }
void CoChannel::setDepth(float d) { depth_ = std::clamp(d, 0.f, 1.f); }

void CoChannel::reset() {
    pll_.reset();
    hil_.reset();
    std::fill(delay_.begin(), delay_.end(), cf32(0, 0));
    std::fill(phasor_.begin(), phasor_.end(), cf32(1, 0));
    dpos_ = 0;
    mAvg_ = 0;
    gain_ = cf32(1, 0);
    refPow_ = inPow_ = outPow_ = 0;
}

cf32 CoChannel::push(cf32 z) {
    if (delay_.empty()) return z;
    const double ph = pll_.phase();
    const cf32 v = pll_.push(z);
    const cf32 nco((float)std::cos(ph), (float)std::sin(ph));   // z = v * nco, so the rebuilt copy goes back the same way
    // the sample and the phasor from the Hilbert filter's delay ago, so the subtraction lines up
    const cf32 zOld = delay_[dpos_], ncoOld = phasor_[dpos_];
    delay_[dpos_] = z;
    phasor_[dpos_] = nco;
    dpos_ = dpos_ + 1 < delay_.size() ? dpos_ + 1 : 0;

    double iDel = 0, qSh = 0;
    hil_.push(v, iDel, qSh);
    // the strong station read off its clean sideband: carrier plus audio, free of an interferer on the other side
    const double est = clean_ == Usb ? iDel - qSh : clean_ == Lsb ? iDel + qSh : iDel;
    mAvg_ += mK_ * (est - mAvg_);
    const cf32 ref = cf32((float)est, 0.f) * ncoOld;            // the whole strong station rebuilt where it was
    const cf32 out = zOld - gain_ * ref * depth_;
    // a normalised least-mean-square step trims the amplitude and phase of the copy, so the subtraction nulls deeply
    refPow_ += powK_ * ((double)std::norm(ref) - refPow_);
    if (pll_.locked() && refPow_ > 1e-18) gain_ += (float)(mu_ / refPow_) * std::conj(ref) * out;
    inPow_ += powK_ * ((double)std::norm(zOld) - inPow_);
    outPow_ += powK_ * ((double)std::norm(out) - outPow_);
    return out;
}

double CoChannel::nullDepthDb() const {
    if (inPow_ <= 1e-20 || outPow_ <= 1e-20) return 0;
    return 10.0 * std::log10(inPow_ / outPow_);
}

} // namespace sam
} // namespace dect2
