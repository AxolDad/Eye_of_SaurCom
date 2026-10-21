#include "dect2/df/group.h"
#include <algorithm>
#include <cmath>

namespace dect2::df {

CoherentGroup::CoherentGroup(std::vector<std::unique_ptr<ChannelFeed>> feeds, double sampleRate, double centerFreq)
    : feeds_(std::move(feeds)), fs_(sampleRate), fc_(centerFreq), pend_(feeds_.size()) {}

bool CoherentGroup::fill(size_t ch, size_t n) {
    auto& p = pend_[ch];
    while (p.size() < n) {
        size_t old = p.size();
        p.resize(n);
        size_t got = feeds_[ch]->read(p.data() + old, n - old);
        p.resize(old + got);
        if (!got) return false;
    }
    return true;
}

bool CoherentGroup::calibrate(size_t n, int maxLag, double minCoherence) {
    int N = channels();
    cal_ = GroupCalibration{};
    if (N < 2 || n < (size_t)(4 * maxLag + 64)) return false;
    for (int c = 0; c < N; c++) { pend_[c].clear(); if (!fill(c, n + 2 * maxLag)) return false; }
    // integer lag of channel c vs channel 0: x_c[t + l] ~ x_0[t]
    std::vector<int> lag(N, 0);
    for (int c = 1; c < N; c++) {
        double best = -1; int bl = 0;
        for (int l = -maxLag; l <= maxLag; l++) {
            cd s = 0;
            for (size_t t = (size_t)maxLag; t < n + maxLag; t++) s += pend_[0][t] * std::conj(pend_[c][t + l]);
            if (std::abs(s) > best) { best = std::abs(s); bl = l; }
        }
        lag[c] = bl;
    }
    int minL = *std::min_element(lag.begin(), lag.end());
    size_t m = SIZE_MAX;
    for (int c = 0; c < N; c++) {   // drop the lead so that all channels line up
        pend_[c].erase(pend_[c].begin(), pend_[c].begin() + (lag[c] - minL));
        m = std::min(m, pend_[c].size());
    }
    Frame x(N);
    for (int c = 0; c < N; c++) x[c].assign(pend_[c].begin(), pend_[c].begin() + m);
    CMat r = covariance(x);
    std::vector<double> ev; std::vector<std::vector<cd>> vec;
    eigh(r, ev, vec);
    double tr = 0; for (double e : ev) tr += e;
    cal_.coherence = tr > 0 ? ev[0] / tr : 0;
    cal_.lagSamples = lag;
    for (auto& l : cal_.lagSamples) l -= lag[0];
    cal_.gains = estimateCalibration(r, Array::uca(N, 0.1), 1.0, std::nan(""));
    cal_.consumed = m;
    cal_.ok = cal_.coherence >= minCoherence;
    for (int c = 0; c < N; c++) pend_[c].erase(pend_[c].begin(), pend_[c].begin() + m);
    return cal_.ok;
}

bool CoherentGroup::read(Frame& out, size_t n) {
    int N = channels();
    out.assign(N, {});
    for (int c = 0; c < N; c++) {
        if (!fill(c, n)) return false;
        out[c].assign(pend_[c].begin(), pend_[c].begin() + n);
        pend_[c].erase(pend_[c].begin(), pend_[c].begin() + n);
        if (cal_.ok) for (auto& z : out[c]) z /= cal_.gains[c];
    }
    return true;
}

}   // namespace dect2::df
