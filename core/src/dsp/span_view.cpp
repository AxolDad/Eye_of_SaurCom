// The Span bar's pure logic (span_view.h)
#include "dect2/dsp/span_view.h"
#include "dect2/dsp/spectrum.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dect2 {

bool parseSpanHz(const std::string& text, double& hz) {
    size_t i = 0;
    const size_t n = text.size();
    auto space = [&] { while (i < n && std::isspace((unsigned char)text[i])) i++; };
    space();
    // the number: digits with at most one decimal point or comma (no sign, no exponent: "1e3" is not a span anyone types)
    double v = 0, scale = 0;
    int digits = 0;
    bool point = false;
    for (; i < n; i++) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            digits++;
            if (point) { v += (c - '0') * scale; scale *= 0.1; }
            else v = v * 10 + (c - '0');
        } else if ((c == '.' || c == ',') && !point) { point = true; scale = 0.1; }
        else break;
    }
    if (digits == 0 || digits > 15) return false;
    space();
    // the unit: [k|m|g] then optionally "hz" or "sps", any case
    std::string u;
    while (i < n && std::isalpha((unsigned char)text[i])) u += (char)std::tolower((unsigned char)text[i++]);
    space();
    if (i != n) return false;
    double mul = 1;
    std::string rest = u;
    if (!u.empty() && (u[0] == 'k' || u[0] == 'm' || u[0] == 'g')) {
        mul = u[0] == 'k' ? 1e3 : u[0] == 'm' ? 1e6 : 1e9;
        rest = u.substr(1);
    }
    if (!(rest.empty() || rest == "hz" || rest == "sps")) return false;
    const double r = v * mul;
    if (!std::isfinite(r) || r <= 0) return false;
    hz = r;
    return true;
}

static std::string threeDigits(double v, const char* unit) {
    char b[48];
    snprintf(b, sizeof b, "%.3g %s", v, unit);
    return b;
}

std::string formatSpan(double hz) {
    if (!(hz > 0) || !std::isfinite(hz)) return "0 Hz";
    if (hz >= 999.5e6) return threeDigits(hz / 1e9, "GHz");
    if (hz >= 999.5e3) return threeDigits(hz / 1e6, "MHz");
    if (hz >= 999.5) return threeDigits(hz / 1e3, "kHz");
    return threeDigits(hz, "Hz");
}

std::string formatResolution(double hzPerBin) {
    return formatSpan(hzPerBin) + "/bin";
}

RatePick rateForSpan(double spanHz, const std::vector<double>& allowedHz, double minHz, double maxHz) {
    RatePick p;
    std::vector<double> r;
    for (double v : allowedHz)
        if (v > 0 && v >= minHz - 1 && (maxHz <= 0 || v <= maxHz + 1)) r.push_back(v);
    if (r.empty()) return p;
    std::sort(r.begin(), r.end());
    p.ok = true;
    const double want = std::max(spanHz, minHz);
    for (double v : r) if (v >= want - 1) { p.rateHz = v; return p; }
    p.rateHz = r.back();
    p.clamped = true;
    return p;
}

int fftForSpan(double spanHz, double rateHz, int minBinsAcross) {
    int n = (int)SpectrumAnalyzer::autoSize(rateHz > 0 ? rateHz : 0);
    if (!(rateHz > 0) || !(spanHz > 0) || spanHz >= rateHz) return n;
    const double need = (double)minBinsAcross * rateHz / spanHz;   // the transform size that puts minBinsAcross bins across the view
    while (n < kSpanMaxFft && n < need) n *= 2;
    return n;
}

OccupiedBins occupiedAround(const float* db, int n, int bin, int searchBins, int gapBins, double aboveDb) {
    OccupiedBins o;
    if (!db || n < 3 || bin < 0 || bin >= n) return o;
    std::vector<float> s(db, db + n);
    std::nth_element(s.begin(), s.begin() + n / 2, s.end());
    o.floorDb = s[n / 2];
    const double thr = o.floorDb + aboveDb;
    int pk = bin;
    for (int i = std::max(0, bin - searchBins); i <= std::min(n - 1, bin + searchBins); i++) if (db[i] > db[pk]) pk = i;
    if (db[pk] < thr) return o;
    gapBins = std::max(1, gapBins);
    auto walk = [&](int dir) {   // the last bin above the threshold before a run of gapBins below it (or the edge)
        int last = pk, below = 0;
        for (int i = pk + dir; i >= 0 && i < n; i += dir) {
            if (db[i] >= thr) { last = i; below = 0; }
            else if (++below >= gapBins) break;
        }
        return last;
    };
    o.lo = walk(-1);
    o.hi = walk(+1);
    o.peak = pk;
    o.valid = true;
    return o;
}

double fitSpanHz(double occupiedHz, double minHz, double factor) {
    return std::max(minHz, occupiedHz * factor);
}

} // namespace dect2
