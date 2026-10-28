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
    int digits = 0, after = 0;
    char sep = 0;
    for (; i < n; i++) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            digits++;
            if (sep) { v += (c - '0') * scale; scale *= 0.1; after++; }
            else v = v * 10 + (c - '0');
        } else if ((c == '.' || c == ',') && !sep) { sep = c; scale = 0.1; }
        else break;
    }
    if (digits == 0 || digits > 15) return false;
    if (sep == ',' && after == 3) return false;   // "1,200": a thousands separator or a decimal comma? Ask again rather than guess
    space();
    // the unit: [k|m|g] then optionally "hz" or "sps", any case; none at all is kHz
    std::string u;
    while (i < n && std::isalpha((unsigned char)text[i])) u += (char)std::tolower((unsigned char)text[i++]);
    space();
    if (i != n) return false;
    double mul = 1e3;
    std::string rest = u;
    if (!u.empty() && (u[0] == 'k' || u[0] == 'm' || u[0] == 'g')) {
        mul = u[0] == 'k' ? 1e3 : u[0] == 'm' ? 1e6 : 1e9;
        rest = u.substr(1);
    } else if (!u.empty()) mul = 1;   // "500 Hz"
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

int fftForSpan(double spanHz, double rateHz, int maxN, int minBinsAcross) {
    int n = (int)SpectrumAnalyzer::autoSize(rateHz > 0 ? rateHz : 0);
    if (!(rateHz > 0) || !(spanHz > 0) || spanHz >= rateHz) return n;
    const double need = (double)minBinsAcross * rateHz / spanHz;   // the transform size that puts minBinsAcross bins across the view
    // doubled while it helps, up to the cap, and while a frame (2n samples) stays within 1/20 s
    while (n < kSpanAutoMaxFft && n < maxN && n < need && 2.0 * n * 20.0 <= rateHz) n *= 2;
    return n;
}

OccupiedBins occupiedAround(const float* db, int n, int bin, const OccupiedOpts& opt) {
    OccupiedBins o;
    if (!db || n < 3 || bin < 0 || bin >= n) return o;
    auto dc = [&](int i) { return opt.dcBin >= 0 && i >= opt.dcBin - opt.dcHalf && i <= opt.dcBin + opt.dcHalf; };
    std::vector<float> s;
    s.reserve(n);
    for (int i = 0; i < n; i++) if (!dc(i)) s.push_back(db[i]);
    if (s.size() < 3) return o;
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    o.floorDb = s[s.size() / 2];
    const double thr = o.floorDb + opt.aboveDb;
    int pk = -1;
    for (int i = std::max(0, bin - opt.searchBins); i <= std::min(n - 1, bin + opt.searchBins); i++)
        if (!dc(i) && (pk < 0 || db[i] > db[pk])) pk = i;
    if (pk < 0 || db[pk] < thr) return o;
    const int gap = std::max(1, opt.gapBins);
    auto above = [&](int i) { return i >= 0 && i < n && !dc(i) && db[i] >= thr; };
    auto walk = [&](int dir) {   // the last bin of the signal before a run of gap bins below the threshold (or the edge)
        int last = pk, below = 0;
        for (int i = pk + dir; i >= 0 && i < n; i += dir) {
            if (dc(i)) continue;   // the DC spike: neither signal nor gap
            if (above(i) && (below == 0 || above(i + dir))) { last = i; below = 0; }   // after a dip, a lone bin above is noise
            else if (++below >= gap) break;
        }
        return last;
    };
    o.lo = walk(-1);
    o.hi = walk(+1);
    // the -6 dB core: no bridging, no DC
    const double coreThr = db[pk] - 6.0;
    o.coreLo = o.coreHi = pk;
    while (o.coreLo - 1 >= 0 && !dc(o.coreLo - 1) && db[o.coreLo - 1] >= coreThr) o.coreLo--;
    while (o.coreHi + 1 < n && !dc(o.coreHi + 1) && db[o.coreHi + 1] >= coreThr) o.coreHi++;
    // at most maxCoreFactor x the core wide (but always minCapBins), about the core's middle
    const int coreW = o.coreHi - o.coreLo + 1;
    const int cap = std::max(opt.minCapBins, (int)std::lround(opt.maxCoreFactor * coreW));
    if (o.hi - o.lo + 1 > cap) {
        const int mid = (o.coreLo + o.coreHi) / 2, h = cap / 2;
        o.lo = std::max(o.lo, std::min(o.coreLo, mid - h));
        o.hi = std::min(o.hi, std::max(o.coreHi, mid + h));
    }
    o.peak = pk;
    o.valid = true;
    return o;
}

double fitSpanHz(double occupiedHz, double minHz, double factor) {
    return std::max(minHz, occupiedHz * factor);
}

} // namespace dect2
