// The Span bar above the spectrum (app/gfx/span_bar.cpp): the pure parts, tested in tests/dsp/test_span_view.cpp.
// A typed span ("50k", "1.2M", "200 kHz"), the radio rate a wider span needs, the FFT size that keeps a narrow view sharp, and the
// occupied width of a signal for "Fit to this signal".
#pragma once
#include <string>
#include <vector>

namespace dect2 {

// "50k", "1.2M", "200 kHz", "2.4 MHz", "6 Msps", "2,4 MHz": a positive, finite width in Hz. Units are k, M and G (any case, so "8m" is
// 8 MHz: nobody asks for a span of millihertz) with an optional "Hz" or "sps"; a bare number is kHz ("500" is 500 kHz, as a ham means
// it). A comma before exactly three digits ("1,200") could be a thousands separator or a decimal comma, so it is refused, as is
// anything else; hz is then left alone.
bool parseSpanHz(const std::string& text, double& hz);

// "2.4 MHz", "50 kHz", "800 Hz": three significant digits, trailing zeros dropped
std::string formatSpan(double hz);
// "293 Hz/bin", "1.22 kHz/bin"
std::string formatResolution(double hzPerBin);

struct RatePick {
    bool ok = false;        // false: nothing to choose (a file, a fixed-rate source, or the mode needs more than the radio gives)
    double rateHz = 0;      // the rate to run the radio at
    bool clamped = false;   // the span asked for is wider than the radio's fastest rate: rateHz is that fastest rate
};
// The smallest rate in allowedHz that holds spanHz, at least minHz (the mode's minimum, 0 = none) and at most maxHz (the radio's fastest,
// 0 = not known). Wider than every allowed rate: the fastest, clamped.
RatePick rateForSpan(double spanHz, const std::vector<double>& allowedHz, double minHz, double maxHz);

// The spectrum FFT size for a view of spanHz out of a capture of rateHz while the FFT size is Auto: the engine's own Auto size at the
// full span (SpectrumAnalyzer::autoSize), larger as the view narrows so at least minBinsAcross bins lie across it; a power of two, at
// most maxN (32768: 65536 stays a manual choice, spectrum.h) and a frame of at most 1/20 s (n <= rateHz / 20), never below the
// engine's own size. maxN lower while the computer is busy.
constexpr int kSpanAutoMaxFft = 32768;
int fftForSpan(double spanHz, double rateHz, int maxN = kSpanAutoMaxFft, int minBinsAcross = 1000);

struct OccupiedBins {
    bool valid = false;
    int lo = 0, hi = 0;           // first and last bin of the signal (inclusive)
    int peak = 0;                 // its strongest bin
    int coreLo = 0, coreHi = 0;   // the -6 dB core around the peak (no bridging)
    double floorDb = 0;           // the noise floor (the median of the spectrum)
};
struct OccupiedOpts {
    int searchBins = 8;           // the peak is looked for within this many bins of the click
    int gapBins = 3;              // dips below the threshold shorter than this are bridged (an AM carrier to its sidebands)...
    double aboveDb = 6.0;         // the threshold: the floor + this
    int dcBin = -1, dcHalf = 2;   // the radio's DC spike: bins dcBin +- dcHalf are neither signal nor a gap (-1: none)
    double maxCoreFactor = 20;    // the signal is at most this many times its -6 dB core wide...
    int minCapBins = 0;           // ...but may always be this wide (a carrier with sidebands: the app passes about 12 kHz)
};
// The signal around bin: its peak within searchBins of the bin, then out to each side while the level stays above the floor + aboveDb.
// After a dip, a bin above the threshold counts only with its neighbour beyond it also above (a lone noise bin does not carry the walk
// on). Not valid on the noise or on the DC spike alone. Use the averaged spectrum the display shows: a raw frame reads wider.
OccupiedBins occupiedAround(const float* db, int n, int bin, const OccupiedOpts& opt = {});

// The view that shows a signal of occupiedHz with its sidebands: factor x its width, at least minHz
double fitSpanHz(double occupiedHz, double minHz = 5e3, double factor = 1.5);

} // namespace dect2
