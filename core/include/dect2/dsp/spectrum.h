#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <vector>

namespace dect2 {

struct SignalStats {
    float peak = 0;        // linear, full scale = 1
    float rmsDbfs = -120;
    float clipFraction = 0; // fraction of samples within 1 LSB of full scale
    float dcI = 0, dcQ = 0;
    uint32_t hist[64] = {}; // |I| and |Q| magnitude histogram (ADC occupancy)
};

// 10*log10(p) for 1e-14 <= p, the fast form the analyser uses (error below 0.0001 dB)
float spectrumDb(float p);

struct SpectrumFrame {
    std::vector<float> dbfs; // fftSize bins, DC-centred (-fs/2 .. +fs/2)
    SignalStats stats;
    uint64_t seq = 0;
};

class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(size_t fftSize = 4096);
    ~SpectrumAnalyzer();
    size_t fftSize() const { return n_; }
    static size_t autoSize(double sampleRate);   // the size that gives bins of about 250 Hz or finer, 8192..32768 (65536 stays a manual choice: about 6 ms a frame on a Pi 4 at 10 Msps)

    // consume samples, averages up to 32 blocks per frame
    void feed(const cf32* x, size_t n);
    bool takeFrame(SpectrumFrame& out); // false if nothing accumulated
    void setTransform(bool on);         // off = stats only
    void resize(size_t fftSize);        // power of two, 1024..65536: a new transform size (drops the accumulated data)
    void reset();                       // forget accumulated data (after retuning)

private:
    struct Impl;
    Impl* p_;
    size_t n_;
};

} // namespace dect2
