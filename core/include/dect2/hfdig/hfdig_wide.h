// Whole-span decoding of the HF digital receiver (the "wide decode" switch, see widespan.h).
// The span (up to kWideMaxSpanHz around the dial frequency) is brought down by one WideDecimator, then
//  - FT8 / FT4 / WSPR: every standard dial slot (kFtxSlots) visible in the span gets its own channel (mix, 24 kHz front end, upper sideband
//    audio) and its own HfdigFtx decoder; the decodes go to the main decoder (setForwardTo) with the slot's dial frequency (FtxDecode::dialHz)
//    and the PSK Reporter spots with the right dial;
//  - RTTY: the span is tiled by upper sideband channels 2150 Hz apart (each 300 .. 3300 Hz), and each has a bank that finds every pair of
//    tones (170 / 425 / 850 Hz shift) in its audio, with one decoder per pair (45.45 / 50 / 75 baud, both polarities, the best one kept).
#pragma once
#include "hfdig_ftx.h"
#include "hfdig_rtty.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace dect2 {

struct FtxSlot { double dialHz; int mode; };       // mode: kFtxModes index
const FtxSlot* ftxSlots(int& count);               // the standard FT8 / FT4 / WSPR dial frequencies of the HF bands (and 6 m)
constexpr double kRttyWideMaxSpanHz = 64000.0;         // RTTY needs a channel per 2150 Hz: the span is limited to keep the CPU load low

class HfdigWide {
public:
    explicit HfdigWide(HfdigFtx& mainFtx);
    ~HfdigWide();
    // dialHz: the radio's tuned frequency; offsetHz: where it sits in the input
    void configure(double inputRate, double spanHz, double dialHz, double offsetHz, bool ftx, bool rtty);
    // Explicit slots (hfdig_multiband.h): only these (dial, mode) pairs, each decoded by its own channel; every one inside the span counts,
    // also one at the reference dialHz. spanHz is the span to cover around dialHz (offsetHz: where dialHz sits in the input).
    void configureSlots(double inputRate, double spanHz, double dialHz, double offsetHz, const std::vector<FtxSlot>& slots);
    void setOffsetHz(double hz);
    void feed(const cf32* x, size_t n);
    void rttyTelemetry(std::vector<RttyWideSignal>& out);   // the receiver thread
    void clearRtty();
    int ftxChannels() const;
    int rttyChannels() const;
    double spanHz() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
