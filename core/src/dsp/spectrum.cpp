#include "dect2/dsp/spectrum.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <mutex>

namespace dect2 {

constexpr size_t kMaxBlocksPerFrame = 16;

// 10*log10(p) for p > 0 as a branch-free loop body the compiler vectorises (std::log10f is a library call per bin: with a 64k FFT that was
// two million calls a second, most of the cost at low sample rates). p = m * 2^e with m in [0.707, 1.414); log2(m) = 2/ln2 * atanh(z),
// z = (m-1)/(m+1), |z| < 0.172, so the series to z^7 is off by under 1e-7 in log2: below 0.00001 dB (checked in tests/dsp/test_spectrum_fastlog.cpp).
static inline float fastDb(float p) {
    uint32_t u; std::memcpy(&u, &p, 4);
    const int e0 = (int)((u >> 23) & 0xFF) - 127;
    u = (u & 0x007FFFFFu) | 0x3F800000u;           // mantissa as m0 in [1, 2)
    float m; std::memcpy(&m, &u, 4);
    const bool hi = m > 1.41421356f;               // fold to [0.707, 1.414)
    m = hi ? m * 0.5f : m;
    const float e = (float)(e0 + (hi ? 1 : 0));
    const float z = (m - 1.f) / (m + 1.f), z2 = z * z;
    const float l2 = 2.88539008f * z * (1.f + z2 * (1.f / 3 + z2 * (1.f / 5 + z2 * (1.f / 7))));
    return 3.01029996f * (e + l2);                 // 10*log10(2) * log2(p)
}

float spectrumDb(float p) { return fastDb(p); }

struct SpectrumAnalyzer::Impl {
    int log2n = 0;
    std::vector<float> window, re, im, accum;
    std::vector<cf32> pending; // partial block carried across feed() calls
    size_t blocks = 0;
    // stats accumulators
    double sumSq = 0, sumI = 0, sumQ = 0;
    float peak = 0;
    uint64_t count = 0, clip = 0;
    uint32_t hist[64] = {};
    uint64_t seq = 0;
    std::atomic<bool> transform{true};
    std::mutex mu;   // the worker thread feeds, the analysis thread takes frames and resets
};

SpectrumAnalyzer::SpectrumAnalyzer(size_t fftSize) : p_(new Impl), n_(fftSize) {
    p_->log2n = (int)std::lround(std::log2((double)n_));
    p_->window.resize(n_);
    hannWindowNorm(p_->window.data(), (int)n_);
    p_->re.resize(n_);
    p_->im.resize(n_);
    p_->accum.assign(n_, 0.f);
}

SpectrumAnalyzer::~SpectrumAnalyzer() {
    delete p_;
}

void SpectrumAnalyzer::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    // Power, DC and peak over every sample, in eight float lanes per piece of 4096 samples (summed into doubles between pieces). The samples
    // are read as interleaved floats (I Q I Q ...) so the loop is plain float arithmetic the compiler vectorises: lanes 0,2,4,6 hold I and
    // 1,3,5,7 hold Q; power is the sum of squares of all lanes. (The complex<float> accessors made it a third slower.)
    // The ADC histogram and the clip count look at every fourth sample, each counted four times: they are display statistics. Four partial
    // histograms in turn: consecutive samples usually fall into the same low bin, and one array would wait on each previous increment.
    for (size_t p0 = 0; p0 < n; p0 += 4096) {
        const size_t pn = std::min<size_t>(4096, n - p0), p4 = pn & ~(size_t)3;   // whole groups of four samples = 8 floats
        const cf32* y = x + p0;
        const float* f = reinterpret_cast<const float*>(y);
        float sq[8] = {}, sm[8] = {}, pk[8] = {};
        for (size_t i = 0; i < p4 * 2; i += 8)
            for (int j = 0; j < 8; j++) {
                const float v = f[i + j];
                sq[j] += v * v; sm[j] += v;
                pk[j] = std::max(pk[j], std::fabs(v));
            }
        for (int j = 0; j < 8; j++) {
            s.sumSq += sq[j]; (j & 1 ? s.sumQ : s.sumI) += sm[j];
            s.peak = std::max(s.peak, pk[j]);
        }
        for (size_t i = p4; i < pn; i++) {
            const float I = y[i].real(), Q = y[i].imag();
            s.sumSq += (double)I * I + (double)Q * Q; s.sumI += I; s.sumQ += Q;
            s.peak = std::max(s.peak, std::max(std::fabs(I), std::fabs(Q)));
        }
        uint32_t h4[4][64] = {};
        uint32_t clip = 0;
        // every fourth sample, in groups of 16 samples (64 input samples): the bins and clip flags are worked out in a plain loop the
        // compiler vectorises, then only the increments run one by one
        size_t i = 0;
        for (; i + 64 <= pn; i += 64) {
            uint8_t bin[32];
            for (int k = 0; k < 16; k++) {
                const float I = std::fabs(y[i + 4 * k].real()), Q = std::fabs(y[i + 4 * k].imag());
                clip += std::max(I, Q) >= 126.0f / 128.0f;
                bin[2 * k] = (uint8_t)std::min(63.f, I * 64);   // clamp as a float first: converting a huge value is undefined (INT_MIN on x86)
                bin[2 * k + 1] = (uint8_t)std::min(63.f, Q * 64);
            }
            for (int k = 0; k < 32; k += 4) { h4[0][bin[k]]++; h4[1][bin[k + 1]]++; h4[2][bin[k + 2]]++; h4[3][bin[k + 3]]++; }
        }
        for (; i < pn; i += 4) {
            const float I = std::fabs(y[i].real()), Q = std::fabs(y[i].imag());
            clip += std::max(I, Q) >= 126.0f / 128.0f;
            h4[0][(int)std::min(63.f, I * 64)]++; h4[0][(int)std::min(63.f, Q * 64)]++;
        }
        s.clip += 4 * (uint64_t)clip;
        for (int b = 0; b < 64; b++) s.hist[b] += 4 * (h4[0][b] + h4[1][b] + h4[2][b] + h4[3][b]);
    }
    s.count += n;

    // display doesn't need every block
    if (!s.transform || s.blocks >= kMaxBlocksPerFrame) { s.pending.clear(); return; }
    // One block: window into split re/im, FFT, add the power. Whole blocks are read straight from the caller's samples; only a partial
    // block at the end is copied (and completed by the next call): the old code copied every sample into a queue and erased the front.
    auto block = [&](const cf32* b) {
        const float* w = s.window.data();
        float* re = s.re.data(); float* im = s.im.data();
        for (size_t i = 0; i < n_; i++) { re[i] = b[i].real() * w[i]; im[i] = b[i].imag() * w[i]; }
        fftSplit(re, im, s.log2n, false);
        float* acc = s.accum.data();
        for (size_t i = 0; i < n_; i++) acc[i] += re[i] * re[i] + im[i] * im[i];
        s.blocks++;
    };
    const cf32* src = x; size_t left = n;
    if (!s.pending.empty()) {   // finish the block left over from the last call
        const size_t take = std::min(left, n_ - s.pending.size());
        s.pending.insert(s.pending.end(), src, src + take);
        src += take; left -= take;
        if (s.pending.size() < n_) return;
        block(s.pending.data());
        s.pending.clear();
    }
    while (left >= n_ && s.blocks < kMaxBlocksPerFrame) { block(src); src += n_; left -= n_; }
    if (s.blocks < kMaxBlocksPerFrame && left) s.pending.assign(src, src + left);
}

void SpectrumAnalyzer::setTransform(bool on) { p_->transform = on; }

size_t SpectrumAnalyzer::autoSize(double fs) {
    size_t n = 8192;
    while (n < 65536 && fs / (double)n > 250.0) n *= 2;
    return n;
}

void SpectrumAnalyzer::resize(size_t fftSize) {
    fftSize = std::min<size_t>(65536, std::max<size_t>(1024, fftSize));
    size_t pw = 1024; while (pw < fftSize) pw *= 2;
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (pw == n_) return;
    n_ = pw;
    s.log2n = (int)std::lround(std::log2((double)n_));
    s.window.resize(n_); hannWindowNorm(s.window.data(), (int)n_);
    s.re.assign(n_, 0.f); s.im.assign(n_, 0.f); s.accum.assign(n_, 0.f);
    s.pending.clear(); s.blocks = 0;
}

void SpectrumAnalyzer::reset() {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    std::fill(s.accum.begin(), s.accum.end(), 0.f);
    s.pending.clear();
    s.blocks = 0; s.sumSq = s.sumI = s.sumQ = 0; s.peak = 0; s.count = s.clip = 0;
    std::fill(std::begin(s.hist), std::end(s.hist), 0u);
}

bool SpectrumAnalyzer::takeFrame(SpectrumFrame& out) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.count == 0 || (s.blocks == 0 && s.transform)) return false;
    out.dbfs.resize(n_);
    // normalise so the sum over bins equals total power (dBFS/bin)
    float inv = 1.0f / ((float)std::max<size_t>(s.blocks, 1) * (float)n_ * (float)n_);
    // DC-centre: the upper half of the FFT output goes first, then the lower half (n_ is a power of two, so the halves are exact)
    const size_t h = n_ / 2;
    float* o = out.dbfs.data(); const float* ac = s.accum.data();
    for (size_t i = 0; i < h; i++) o[i] = fastDb(std::max(ac[i + h] * inv, 1e-14f));
    for (size_t i = h; i < n_; i++) o[i] = fastDb(std::max(ac[i - h] * inv, 1e-14f));
    out.stats.peak = s.peak;
    out.stats.rmsDbfs = 10.0f * std::log10(std::max(s.sumSq / (double)s.count, 1e-12));
    out.stats.clipFraction = (float)s.clip / (float)s.count;
    out.stats.dcI = (float)(s.sumI / s.count);
    out.stats.dcQ = (float)(s.sumQ / s.count);
    std::copy(std::begin(s.hist), std::end(s.hist), out.stats.hist);
    out.seq = ++s.seq;

    std::fill(s.accum.begin(), s.accum.end(), 0.f);
    s.blocks = 0; s.sumSq = s.sumI = s.sumQ = 0; s.peak = 0; s.count = s.clip = 0;
    std::fill(std::begin(s.hist), std::end(s.hist), 0u);
    return true;
}

} // namespace dect2
