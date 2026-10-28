// The Span bar above the spectrum (app/gfx/span_bar.cpp): the pure parts, tested in tests/dsp/test_span_view.cpp.
// A typed span ("50k", "1.2M", "200 kHz"), the radio rate a wider span needs, the FFT size that keeps a narrow view sharp, and the
// occupied width of a signal for "Fit to this signal".
#pragma once
#include <string>
#include <vector>

namespace dect2 {

// "50k", "1.2M", "200 kHz", "2.4 MHz", "500", "6 Msps", "2,4 MHz": a positive, finite width in Hz. Units are k, M and G (any case, so
// "8m" is 8 MHz: nobody asks for a span of millihertz) with an optional "Hz" or "sps". Anything else is refused and hz is left alone.
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
// most 65536.
int fftForSpan(double spanHz, double rateHz, int minBinsAcross = 1000);
constexpr int kSpanMaxFft = 65536;

struct OccupiedBins {
    bool valid = false;
    int lo = 0, hi = 0;     // first and last bin of the signal (inclusive)
    int peak = 0;           // its strongest bin
    double floorDb = 0;     // the noise floor (the median of the spectrum)
};
// The signal around bin: its peak within searchBins of the bin, then out to each side while the level stays above the floor + aboveDb;
// dips below that shorter than gapBins are bridged (the gap between an AM carrier and its sidebands). Not valid on the noise.
OccupiedBins occupiedAround(const float* db, int n, int bin, int searchBins, int gapBins = 3, double aboveDb = 6.0);

// The view that shows a signal of occupiedHz with its sidebands: factor x its width, at least minHz
double fitSpanHz(double occupiedHz, double minHz = 5e3, double factor = 1.5);

} // namespace dect2
