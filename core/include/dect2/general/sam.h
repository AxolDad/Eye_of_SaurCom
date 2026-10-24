// Synchronous AM and the co-channel canceller, on the General receiver's channel stream (complex baseband at the demodulation rate).
//
// Synchronous AM (SAM) locks a phase-locked loop to the station's own carrier and takes the in-phase part, instead of the envelope an
// ordinary AM detector measures. Two things get better: fading that takes the carrier down no longer distorts the audio (the loop rides
// through it), and either sideband can be listened to on its own, which steps around an interfering station sitting on one side.
//
// The co-channel canceller is for two AM stations on one channel, the far one buried under the near one. It locks to the strong
// carrier, estimates that station's audio from whichever of its sidebands the interference is not on, rebuilds the strong signal from
// that estimate (carrier and both sidebands) and subtracts it. An adaptive complex gain trims the rebuilt copy so the subtraction
// nulls deeply. What is left is the weak station, which the demodulator after it can read. See docs/general-sam.md.
//
// No allocation after configure(); every class is one sample in, one sample out, usable from the analysis thread.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <vector>

namespace dect2 {
namespace sam {

// Which sideband to listen to (SAM), or which one is free of the interference (the canceller).
enum Side { Dsb = 0, Usb, Lsb };
const char* sideName(int side);

// ---- carrier loop ----------------------------------------------------------------------------------------------------------------
// A second-order phase-locked loop that follows an amplitude-modulated carrier. The error is the phase of the derotated sample, which
// is the right detector while the carrier is there at all (AM keeps it, unlike a suppressed-carrier signal).
class CarrierPll {
public:
    // loopBwHz: how fast the loop follows (3 - 20 Hz suits broadcast AM; wider catches a drifting carrier, narrower rides through fading).
    // pullInHz: how far from the channel centre the loop may hunt for the carrier.
    void configure(double rateHz, double loopBwHz = 10.0, double pullInHz = 200.0);
    void reset();
    // One channel sample. Returns it derotated, so a locked carrier sits at 0 Hz with its phase at 0: the real part is then the
    // carrier plus the audio, and the imaginary part is what the loop has not matched.
    cf32 push(cf32 z);
    double offsetHz() const { return freq_ * rate_ / (2.0 * 3.14159265358979323846); }   // where the carrier is, relative to the channel
    double amplitude() const { return amp_; }     // the carrier's own amplitude, smoothed
    double lock() const { return lock_; }         // 0 (nothing) to 1 (the loop is on the carrier)
    bool locked() const { return lock_ > 0.7; }
    double phase() const { return phase_; }
private:
    double rate_ = 48000, alpha_ = 0, beta_ = 0, maxFreq_ = 0;
    double wideAlpha_ = 0, wideBeta_ = 0;     // while hunting, the loop runs wider so it can pull a carrier in from further away
    double phase_ = 0, freq_ = 0, amp_ = 0, lock_ = 0, ampK_ = 0, lockK_ = 0;
};

// ---- 90 degree phase shift -------------------------------------------------------------------------------------------------------
// A Hilbert transformer: one arm is shifted by 90 degrees and the other delayed to match, which is what splits a complex channel into
// its two sidebands (upper = I - H{Q}, lower = I + H{Q}).
class Hilbert90 {
public:
    void configure(int halfTaps = 31);   // the filter is 2 * halfTaps + 1 long; its delay is halfTaps samples
    void reset();
    // Takes one complex sample and gives the delayed in-phase part and the shifted quadrature part.
    void push(cf32 z, double& iDelayed, double& qShifted);
    int delaySamples() const { return half_; }
    bool ready() const { return !h_.empty(); }
private:
    std::vector<double> h_;        // only the odd taps are non-zero
    std::vector<double> bufI_, bufQ_;
    size_t pos_ = 0;
    int half_ = 0;
};

// ---- synchronous AM --------------------------------------------------------------------------------------------------------------
class SamDemod {
public:
    void configure(double rateHz, double loopBwHz = 10.0);
    void setSide(int side);                       // Dsb, Usb or Lsb
    void reset();
    // One channel sample in, the audio sample out (the carrier removed, so silence gives 0). The level follows the modulation depth.
    float push(cf32 z);
    const CarrierPll& pll() const { return pll_; }
    int side() const { return side_; }
private:
    CarrierPll pll_;
    Hilbert90 hil_;
    int side_ = Dsb;
    double dc_ = 0, dcK_ = 0;
};

// ---- co-channel canceller --------------------------------------------------------------------------------------------------------
class CoChannel {
public:
    // cleanSide: the sideband of the strong station that the interference is not on; its audio is estimated from there.
    void configure(double rateHz, double loopBwHz = 10.0);
    void setCleanSide(int side);                  // Usb or Lsb (Dsb: both, for a station with no interference on either side)
    void setDepth(float d);                       // 0 off (passes the input through) to 1 full subtraction
    void reset();
    // One channel sample in; the strong station taken out of it. Feed the result to a demodulator to hear the weak station.
    cf32 push(cf32 z);
    int delaySamples() const { return hil_.delaySamples(); }   // the output is this many samples behind the input
    double strongOffsetHz() const { return pll_.offsetHz(); }
    double strongLock() const { return pll_.lock(); }
    bool locked() const { return pll_.locked(); }
    // How far the strong station has been pushed down, dB (the power of what was removed against what is left), smoothed.
    double nullDepthDb() const;
private:
    CarrierPll pll_;
    Hilbert90 hil_;
    int clean_ = Usb;
    float depth_ = 1.f;
    double rate_ = 48000;
    double mAvg_ = 0, mK_ = 0;        // the strong station's audio estimate, direct current removed
    cf32 gain_{1.f, 0.f};             // the adaptive complex gain of the rebuilt copy
    double mu_ = 0;
    double inPow_ = 0, outPow_ = 0, powK_ = 0;
    // the input and the loop's phasor are delayed to line up with the Hilbert arm, so the subtraction is sample-accurate
    std::vector<cf32> delay_, phasor_;
    size_t dpos_ = 0;
    double refPow_ = 0;
};

} // namespace sam
} // namespace dect2
