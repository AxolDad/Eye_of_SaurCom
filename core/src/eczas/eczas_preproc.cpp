// BFO audio (~1 kHz tone with PCSK225) to 500 Hz discriminator PCM: the whole-file filter chain of sp5wwp/e-Czas (https://github.com/sp5wwp/e-Czas, GPL-3.0;
// Eye of SaurCom is GPL-3.0 too, THIRD_PARTY.md), kept for the offline decoder and the tests. The app uses the streaming front end of eczas_live.cpp.
#include "dect2/eczas/eczas_rx.h"
#include "dect2/dsp/fftutil.h"
#include "eczas_taps.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2::eczas {

namespace {

using dect2::math::kPi;
constexpr double kDeviation = 36.0 / (360.0 * 0.019);
constexpr int kHilN = 101;
constexpr float kShortScale = 16384.f;

struct SosStage {
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(const double row[6]) {
        const double a0 = row[3];
        b0 = row[0] / a0;
        b1 = row[1] / a0;
        b2 = row[2] / a0;
        a1 = row[4] / a0;
        a2 = row[5] / a0;
        z1 = z2 = 0;
    }
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct SosChain {
    SosStage st[8];
    int n = 0;
    void load(const double sos[][6], int stages) {
        n = stages;
        for (int i = 0; i < stages; i++) st[i].set(sos[i]);
    }
    double run(double x) {
        for (int i = 0; i < n; i++) x = st[i].run(x);
        return x;
    }
    void runBuffer(std::vector<double>& x) {
        for (double& v : x) v = run(v);
    }
};

struct RatePreset {
    const double (*bp)[6];
    int bpStages;
    const double (*lp)[6];
    int lpStages;
    int decim;
};

RatePreset presetFor(double sr) {
    if (std::fabs(sr - 48000.0) < 1.0) return {kSr48BpSos, 6, kSr48LpSos, 2, kSr48Decim};
    if (std::fabs(sr - 8000.0) < 1.0) return {kSr8BpSos, 6, kSr8LpSos, 2, kSr8Decim};
    const int decim = (int)std::lround(sr / kDiscRateHz);
    if (decim == kSr8Decim) return {kSr8BpSos, 6, kSr8LpSos, 2, kSr8Decim};
    if (decim == kSr48Decim) return {kSr48BpSos, 6, kSr48LpSos, 2, kSr48Decim};
    return {kSr8BpSos, 6, kSr8LpSos, 2, std::max(decim, 1)};
}

static void scipyHilbert(const std::vector<double>& y, std::vector<std::complex<double>>& z) {
    const size_t n = y.size();
    z.resize(n);
    if (n == 0) return;
    int log2n = 0;
    while ((size_t)(1 << log2n) < n) log2n++;
    const int N = 1 << log2n;
    std::vector<cf32> buf((size_t)N);
    for (int i = 0; i < N; i++) buf[i] = (i < (int)n) ? cf32((float)y[(size_t)i], 0.f) : cf32(0.f, 0.f);
    Fft fft(N);
    fft.forward(buf.data());
    for (int i = 0; i < N; i++) {
        float scale = 0.f;
        if (i == 0) scale = 1.f;
        else if (N % 2 == 0 && i == N / 2) scale = 1.f;
        else if (i < N / 2) scale = 2.f;
        buf[i] = cf32(buf[i].real() * scale, buf[i].imag() * scale);
    }
    fft.inverse(buf.data());
    for (size_t i = 0; i < n; i++) z[i] = {buf[i].real(), buf[i].imag()};
}

} // namespace

std::vector<int16_t> eczasAudioToDisc(const int16_t* in, size_t n, double sr) {
    const RatePreset p = presetFor(sr);
    SosChain bp, bp2, lp;
    bp.load(p.bp, p.bpStages);
    bp2.load(p.bp, p.bpStages);
    lp.load(p.lp, p.lpStages);

    std::vector<double> y(n);
    for (size_t i = 0; i < n; i++) y[i] = (in[i] / 32768.0);
    bp.runBuffer(y);
    for (double& v : y) v *= 10.0;
    double energy = 0;
    for (double v : y) energy += v * v;
    const double agcRef = 1.0 / std::sqrt(2.0);
    const double g = agcRef / std::sqrt(energy / (double)y.size() + 1e-18);
    for (double& v : y) v *= g;
    bp2.runBuffer(y);

    std::vector<std::complex<double>> analytic;
    scipyHilbert(y, analytic);

    const int maLen = (int)std::lround(sr / 10.0);
    const int delayLen = (int)std::lround(sr / 20.0);
    const double demodGain = sr / (2.0 * kPi * kDeviation);
    std::vector<double> dem(n, 0.0);
    std::complex<double> zPrev = analytic.empty() ? std::complex<double>{1, 0} : analytic[0];
    for (size_t i = 1; i < n; i++) {
        const auto prod = analytic[i] * std::conj(analytic[i - 1]);
        dem[i] = demodGain * std::atan2(prod.imag(), prod.real());
    }
    dem[0] = dem.size() > 1 ? dem[1] : 0;

    std::vector<double> ma(n, 0.0);
    const int half = maLen / 2;
    for (size_t i = 0; i < n; i++) {
        double sum = 0;
        for (int k = 0; k < maLen; k++) {
            const ptrdiff_t idx = (ptrdiff_t)i - half + k;
            if (idx >= 0 && idx < (ptrdiff_t)n) sum += dem[(size_t)idx];
        }
        ma[i] = sum / (double)maLen;
    }
    std::vector<double> edge(n, 0.0);
    for (size_t i = 0; i < n; i++) {
        const double delayed = (i >= (size_t)delayLen) ? dem[i - (size_t)delayLen] : 0.0;
        edge[i] = delayed - ma[i];
    }
    lp.runBuffer(edge);

    std::vector<int16_t> out;
    out.reserve(n / (size_t)p.decim + 16);
    for (size_t i = 0; i < n; i += (size_t)p.decim) {
        const float v = (float)edge[i] * kShortScale;
        const int s = (int)std::lround(std::clamp(v, -32768.f, 32767.f));
        out.push_back((int16_t)s);
    }
    return out;
}

std::vector<EczasRxPacket> eczasDecodeRecording(const int16_t* pcm, size_t n, double pcmRateHz) {
    if (pcmRateHz <= kDiscRateHz * 1.5) return decodeDiscPcm(pcm, n, pcmRateHz, 0);
    const std::vector<int16_t> disc = eczasAudioToDisc(pcm, n, pcmRateHz);
    return decodeDiscPcm(disc.data(), disc.size(), kDiscRateHz, 0);
}

} // namespace dect2::eczas
