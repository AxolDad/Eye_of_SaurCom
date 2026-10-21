// Multi-band monitor for FT8, FT4, FT2 and WSPR: the user picks (band, mode) pairs (160 m .. 6 m), and every pair that lies inside the
// radio's capture is decoded at once, whatever the radio: an RTL-SDR V4 (2.4 MHz) tuned to 14 MHz hears 20 m FT8, FT4, FT2 and WSPR,
// a wideband radio (Airspy HF+, SDRplay, Hermes Lite 2, HackRF, RX888) hears every selected band.
//
//   feed(capture) -> per band: HfdigWide (WideDecimator + one 12 kHz-class chain per dial frequency) -> HfdigFtx slot decoders
//                 -> the main HfdigFtx (one combined list, FtxDecode::dialHz is the true dial frequency of the decode)
//
// The slot clock is the main decoder's (copyClockFrom): FT8 15 s, FT4 7.5 s, FT2 3.75 s, WSPR 120 s, all from the same clock.
// Pure logic (no DSP) lives in the free functions: the band table, the dial frequencies, coverage and the best centre, and the CPU guard.
// Receive only.
#pragma once
#include "hfdig_ftx.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace dect2 {

constexpr int kMbBands = 11;                 // 160 80 60 40 30 20 17 15 12 10 6 m
constexpr int kMbModes = 4;                  // FT8 FT4 FT2 WSPR (kFtx mode numbers 0 1 2 3)
struct MbBand { const char* name; double loHz, hiHz; };
const MbBand& mbBand(int band);
// The dial frequency of the mode in the band (Hz), 0 when there is none (FT4 has no 160 m / 80 m... dial in the table, FT2 only on 20 m)
double mbDialHz(int band, int mode);

struct MbSel { int band; int mode; };        // band 0 .. kMbBands-1, mode 0 .. 3
inline bool operator==(const MbSel& a, const MbSel& b) { return a.band == b.band && a.mode == b.mode; }
// A selection as a bit mask (band * 4 + mode) for the preferences
uint64_t mbMask(const std::vector<MbSel>& s);
std::vector<MbSel> mbFromMask(uint64_t m);

constexpr double kMbAudioHz = 3300.0;        // the 3 kHz of a slot's audio must lie inside the span
constexpr double kMbUsableFrac = 0.80;       // the usable part of a capture (the filter edges of the radio are not)

struct MbCover {
    MbSel sel;
    double dialHz = 0;
    bool covered = false;
    double neededSpanHz = 0;                 // when not covered: the capture width that would hold it together with the centre (>= 2 |dial - centre| + 6.6 kHz) / usable
    double offsetHz = 0;                     // dial - centre
};
// Which selections the capture (centre, sample rate) holds. Selections without a dial frequency are listed as not covered, dialHz 0.
std::vector<MbCover> mbCoverage(double centreHz, double rateHz, const std::vector<MbSel>& sels);
// The centre that covers the most selections (ties: the lowest centre); `covered` gets the count. The frequency is the middle of the window.
double mbBestCentre(double rateHz, const std::vector<MbSel>& sels, int* covered = nullptr);
// The sample rate that would hold all the selections at one centre (what to tell the user: "needs 12 MHz")
double mbNeededRate(const std::vector<MbSel>& sels);

// CPU guard: feed the measured processing time against the audio time of the same block; when the load stays above the limit the guard
// asks for one selection (the least useful) to be dropped; when it stays low it allows trying again.
class MbCpuGuard {
public:
    explicit MbCpuGuard(double limit = 0.85, int blocksToAct = 8) : limit_(limit), n_(blocksToAct) {}
    // true: drop one more (the load has been over the limit for blocksToAct blocks in a row)
    bool update(double busySec, double blockSec);
    double load() const { return load_; }
    bool tooSlow() const { return slow_; }
    void reset() { load_ = 0; over_ = 0; slow_ = false; }
private:
    double limit_, load_ = 0;
    int n_, over_ = 0;
    bool slow_ = false;
};

struct MbStatus {
    int bands = 0, channels = 0;             // bands with a decimator, slot decoders running
    int dropped = 0;                         // selections given up by the CPU guard
    bool tooSlow = false;
    double load = 0;                         // processing time / real time, smoothed
    std::vector<MbSel> active, notCovered, droppedSel;
};

class HfdigMultiBand {
public:
    explicit HfdigMultiBand(HfdigFtx& mainFtx);
    ~HfdigMultiBand();
    // centreHz: the radio's centre frequency (offsetHz = the dial frequency's place in the input is derived per band)
    void configure(double rateHz, double centreHz, const std::vector<MbSel>& sels);
    void feed(const cf32* x, size_t n);
    MbStatus status() const;
    void setLoadClock(double (*clock)());    // tests: replaces the wall clock used to measure the load
    void testForceLoad(double busySec, double blockSec);   // tests: one guard step with an invented load
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
