// Shared tools of JT65, JT9, Q65, FST4 and MSK144 (see hfdig_wsjt_int.h): resampling of the 8 kHz audio, spectrograms, tone correlation, and
// the 72-bit message packing of JT65 / JT9 (WSJT-X lib/packjt.f90: 28-bit calls, 15-bit locators, 13 characters of free text).
#include "hfdig_wsjt_int.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "dect2/common/window.h"

namespace dect2 {
namespace wsjt {

namespace {

double besselI0(double x) { return dect2::window::besselI0(x, 60, 1e-14); }

} // namespace

std::vector<float> resample8k(const float* x, size_t n, double rateOut) {
    constexpr int kHalf = 32, kPhases = 512;
    const double cut = std::min(3700.0, 0.46 * rateOut) / 8000.0;   // cycles per input sample
    std::vector<float> tab((size_t)(kPhases + 1) * 2 * kHalf);
    for (int ph = 0; ph <= kPhases; ph++)
        for (int k = 0; k < 2 * kHalf; k++) {
            const double u = (k - kHalf + 1) - (double)ph / kPhases;    // input offset from the output instant: tap i0 + k - kHalf + 1
            const double w = std::fabs(u) < kHalf ? besselI0(8.0 * std::sqrt(1 - (u / kHalf) * (u / kHalf))) / besselI0(8.0) : 0.0;
            const double a = 2 * cut * u * M_PI;
            tab[(size_t)ph * 2 * kHalf + (size_t)k] = (float)(2 * cut * (std::fabs(a) < 1e-9 ? 1.0 : std::sin(a) / a) * w);
        }
    const size_t m = n ? (size_t)std::floor((double)(n - 1) * rateOut / 8000.0) + 1 : 0;
    std::vector<float> y(m, 0.f);
    const double step = 8000.0 / rateOut;
    for (size_t j = 0; j < m; j++) {
        const double t = (double)j * step;
        const long i0 = (long)std::floor(t);
        const double fr = t - (double)i0;
        const double ph = fr * kPhases;
        const int p0 = (int)ph;
        const float a = (float)(ph - p0);
        const float* t0 = &tab[(size_t)p0 * 2 * kHalf];
        const float* t1 = t0 + 2 * kHalf;
        float acc = 0;
        for (int k = 0; k < 2 * kHalf; k++) {
            const long i = i0 + k - kHalf + 1;
            if (i < 0 || i >= (long)n) continue;
            acc += x[i] * (t0[k] + a * (t1[k] - t0[k]));
        }
        y[j] = acc;
    }
    return y;
}

Spectrogram spectrogram(const std::vector<float>& y, int N, int hop, int binLo, int binHi, bool hann) {
    Spectrogram s;
    s.N = N; s.hop = hop; s.binLo = binLo; s.nBins = std::max(0, binHi - binLo);
    s.frames = y.size() >= (size_t)N ? (int)((y.size() - (size_t)N) / (size_t)hop) + 1 : 0;
    s.p.assign((size_t)s.frames * (size_t)s.nBins, 0.f);
    Fft fft(N);
    std::vector<cf32> buf((size_t)N);
    std::vector<float> win;   // Hann: a strong signal splashes over the whole band with a rectangular window (the sync search of JT65 B and C was fooled by that)
    if (hann) { win.resize((size_t)N); for (int i = 0; i < N; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / N)); }
    for (int f = 0; f < s.frames; f++) {
        const float* in = &y[(size_t)f * (size_t)hop];
        for (int i = 0; i < N; i++) buf[(size_t)i] = cf32(hann ? in[i] * win[(size_t)i] : in[i], 0.f);
        fft.forward(buf.data());
        float* o = &s.p[(size_t)f * (size_t)s.nBins];
        for (int b = 0; b < s.nBins; b++) o[b] = std::norm(buf[(size_t)(binLo + b)]);
    }
    return s;
}

double toneEnergy(const std::vector<float>& y, double rate, long start, int n, double freq) {
    const double w = 2 * M_PI * freq / rate;
    const long i0 = std::max(0L, -start), i1 = std::min((long)n, (long)y.size() - start);
    if (i1 <= i0) return 0;
    const float cs = (float)std::cos(w), sn = (float)std::sin(w);
    double ar = 0, ai = 0;
    constexpr int kBlock = 64;
    for (long b = i0; b < i1; b += kBlock) {
        const long e = std::min(i1, b + kBlock);
        float cr = (float)std::cos(w * (double)b), ci = (float)-std::sin(w * (double)b);
        const float* p = &y[(size_t)(start + b)];
        float sr = 0, si = 0;
        for (long k = 0; k < e - b; k++) {
            sr += p[k] * cr; si += p[k] * ci;
            const float t = cr * cs + ci * sn;   // rotate by -w: (cr + j ci) * (cs - j sn)
            ci = ci * cs - cr * sn;
            cr = t;
        }
        ar += sr; ai += si;
    }
    return ar * ar + ai * ai;
}

void symbolPowers(const std::vector<float>& y, double rate, long start, int N, double shiftHz, int binLo, int nb, float* out) {
    thread_local Fft* fft = nullptr;
    thread_local int fftN = 0;
    if (fftN != N) { delete fft; fft = new Fft(N); fftN = N; }
    std::vector<cf32> buf((size_t)N);
    const double w = 2 * M_PI * shiftHz / rate;
    const std::complex<double> st(std::cos(w), -std::sin(w));
    std::complex<double> rot(1, 0);
    for (int i = 0; i < N; i++) {
        if ((i & 255) == 0) rot = std::polar(1.0, -w * i);
        const long k = start + i;
        const float v = k >= 0 && k < (long)y.size() ? y[(size_t)k] : 0.f;
        buf[(size_t)i] = cf32((float)(rot.real() * v), (float)(rot.imag() * v));
        rot *= st;
    }
    fft->forward(buf.data());
    for (int b = 0; b < nb; b++) {
        const int k = ((binLo + b) % N + N) % N;
        out[b] = std::norm(buf[(size_t)k]);
    }
}

void addFsk(const std::vector<double>& freqs, double symSec, double amp, double rate, double start, std::vector<float>& out) {
    const double total = (double)freqs.size() * symSec;
    long n0 = (long)std::floor(start * rate), n1 = (long)std::ceil((start + total) * rate);
    n0 = std::max(n0, 0L); n1 = std::min(n1, (long)out.size());
    std::vector<double> cum(freqs.size() + 1, 0.0);   // cycles at the start of each symbol
    for (size_t j = 0; j < freqs.size(); j++) cum[j + 1] = cum[j] + freqs[j] * symSec;
    for (long n = n0; n < n1; n++) {
        const double t = (double)n / rate - start;
        const long k = (long)std::floor(t / symSec);
        if (k < 0 || k >= (long)freqs.size()) continue;
        const double ph = cum[(size_t)k] + freqs[(size_t)k] * (t - (double)k * symSec);
        out[(size_t)n] += (float)(amp * std::sin(2 * M_PI * ph));
    }
}

double logI0(double x) {
    x = std::fabs(x);
    if (x < 3.75) {
        const double t = (x / 3.75) * (x / 3.75);
        return std::log(1 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 + t * (0.2659732 + t * (0.0360768 + t * 0.0045813))))));
    }
    const double t = 3.75 / x;
    const double p = 0.39894228 + t * (0.01328592 + t * (0.00225319 + t * (-0.00157565 + t * (0.00916281 + t * (-0.02057706 +
                     t * (0.02635537 + t * (-0.01647633 + t * 0.00392377)))))));
    return x - 0.5 * std::log(x) + std::log(p);
}

void symbolPowersPad(const std::vector<float>& y, double rate, long start, int nwin, int Nfft, double shiftHz, int binLo, int nb, float* out) {
    thread_local Fft* fft = nullptr;
    thread_local int fftN = 0;
    if (fftN != Nfft) { delete fft; fft = new Fft(Nfft); fftN = Nfft; }
    std::vector<cf32> buf((size_t)Nfft, cf32(0.f, 0.f));
    const double w = 2 * M_PI * shiftHz / rate;
    const std::complex<double> st(std::cos(w), -std::sin(w));
    std::complex<double> rot(1, 0);
    for (int i = 0; i < nwin; i++) {
        if ((i & 255) == 0) rot = std::polar(1.0, -w * i);
        const long k = start + i;
        const float v = k >= 0 && k < (long)y.size() ? y[(size_t)k] : 0.f;
        buf[(size_t)i] = cf32((float)(rot.real() * v), (float)(rot.imag() * v));
        rot *= st;
    }
    fft->forward(buf.data());
    for (int b = 0; b < nb; b++) {
        const int k = ((binLo + b) % Nfft + Nfft) % Nfft;
        out[b] = std::norm(buf[(size_t)k]);
    }
}

double driftSearch(const std::vector<float>& y, double rate, int N, const std::vector<int8_t>& syncTone, double toneStep, long start, double& f, double df) {
    const int nsym = (int)syncTone.size();
    const double T = (double)nsym * N / rate;
    constexpr int W = 9;   // bins either side of the fitted frequency
    const int b0 = (int)std::lround(f / df);
    const double fit = f - b0 * df;
    // the powers of the bins around each sync tone (the first sync tone of the symbol is what is looked at: tone * toneStep / df bins up)
    std::vector<std::vector<float>> pw;
    std::vector<double> pos;
    for (int i = 0; i < nsym; i++) {
        if (syncTone[(size_t)i] < 0) continue;
        std::vector<float> o((size_t)(2 * W + 1));
        const int toneBin = (int)std::lround(syncTone[(size_t)i] * toneStep / df);
        symbolPowers(y, rate, start + (long)i * N, N, fit, b0 + toneBin - W, 2 * W + 1, o.data());
        pw.push_back(std::move(o));
        pos.push_back((double)i / std::max(1, nsym - 1) - 0.5);
    }
    if (pw.size() < 8) return 0.0;
    // the track of the tone: offset(i) = a + D pos(i) bins; the sum of the powers along it (linear interpolation between the bins)
    auto score = [&](double a, double D) {
        double sc = 0;
        for (size_t k = 0; k < pw.size(); k++) {
            const double o = a + D * pos[k] + W;
            const int i0 = (int)std::floor(o);
            if (i0 < 0 || i0 + 1 > 2 * W) continue;
            const double u = o - i0;
            sc += (1 - u) * pw[k][(size_t)i0] + u * pw[k][(size_t)i0 + 1];
        }
        return sc;
    };
    double s0 = 0, a0 = 0;
    for (double a = -4; a <= 4.01; a += 0.5) { const double v = score(a, 0); if (v > s0) { s0 = v; a0 = a; } }
    double best = s0, ba = a0, bD = 0;
    for (double D = -6; D <= 6.01; D += 0.5) {
        if (D == 0) continue;
        for (double a = -4; a <= 4.01; a += 0.5) { const double v = score(a, D); if (v > best) { best = v; ba = a; bD = D; } }
    }
    if (bD != 0) for (double dd : {0.25, -0.25}) for (double da : {0.25, -0.25, 0.0}) { const double v = score(ba + da, bD + dd); if (v > best) { best = v; ba += da; bD += dd; } }
    if (wsjtDebug()) printf("  drift search: best %.2f bins over the transmission at %.2f bins (E %.3g against %.3g with none at %.2f)\n", bD, ba, best, s0, a0);
    if (bD == 0 || best < 1.15 * s0) { f += a0 * df; return 0.0; }
    f += ba * df;
    return bD * df / (f * T);
}

std::vector<float> warpDrift(const std::vector<float>& y, double eps, double mid, double rate) {
    std::vector<float> out(y.size(), 0.f);
    auto at = [&](long k) { return k >= 0 && k < (long)y.size() ? (double)y[(size_t)k] : 0.0; };
    for (size_t n = 0; n < y.size(); n++) {
        const double tn = ((double)n - mid) / rate;                     // the new time about the middle, s
        const double ts = (std::sqrt(std::max(1e-9, 1 + 2 * eps * tn)) - 1) / eps;   // the old time about the middle
        const double pos = mid + ts * rate;
        const long i = (long)std::floor(pos);
        const double u = pos - (double)i;
        const double p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        out[n] = (float)(p1 + 0.5 * u * (p2 - p0 + u * (2 * p0 - 5 * p1 + 4 * p2 - p3 + u * (3 * (p1 - p2) + p3 - p0))));
    }
    return out;
}

float lowMean(std::vector<float> v, double frac) {
    if (v.empty()) return 0;
    const size_t k = std::max<size_t>(1, (size_t)((double)v.size() * frac));
    std::nth_element(v.begin(), v.begin() + (long)k - 1, v.end());
    double s = 0;
    for (size_t i = 0; i < k; i++) s += v[i];
    return (float)(s / (double)k);
}

std::vector<Cand> syncScan(const Spectrogram& sg, const std::vector<int8_t>& syncTone, int mult, int fps, int lagLo, int lagHi, int nbSearch, float minScore,
                           size_t maxCand, int hw) {
    std::vector<Cand> out;
    if (sg.frames <= 0 || lagHi < lagLo) return out;
    // the noise level of every bin: the mean of the lower half of the column over all frames, scaled to the mean of an exponential variable
    std::vector<float> inv((size_t)sg.nBins);
    std::vector<float> col((size_t)sg.frames);
    for (int b = 0; b < sg.nBins; b++) {
        for (int f = 0; f < sg.frames; f++) col[(size_t)f] = sg.p[(size_t)f * (size_t)sg.nBins + (size_t)b];
        inv[(size_t)b] = 1.0f / (lowMean(col, 0.5) / 0.3069f + 1e-30f);
    }
    int ns = 0, maxTone = 0;
    for (int8_t t : syncTone) if (t >= 0) { ns++; maxTone = std::max<int>(maxTone, t); }
    if (!ns) return out;
    const int nb = std::min(nbSearch, sg.nBins - maxTone * mult);
    if (nb <= 0) return out;
    std::vector<float> best((size_t)nb, -1e9f);
    std::vector<int> bestLag((size_t)nb, 0);
    // drift hypotheses: the tone moves by D bins over the transmission (linear, about its middle)
    static const double driftBins[5] = {0, 1.5, -1.5, 3.0, -3.0};
    const int nsym = (int)syncTone.size();
    for (double dr : driftBins) {
        std::vector<int> shiftOf((size_t)nsym, 0);
        for (int i = 0; i < nsym; i++) shiftOf[(size_t)i] = (int)std::lround(dr * ((double)i / std::max(1, nsym - 1) - 0.5));
        for (int lag = lagLo; lag <= lagHi; lag++) {
            if (lag < 0) continue;
            for (int b = 0; b < nb; b++) {
                double a = 0;
                bool ok = true;
                for (size_t i = 0; i < syncTone.size(); i++) {
                    if (syncTone[i] < 0) continue;
                    const int f = lag + fps * (int)i;
                    if (f >= sg.frames) { ok = false; break; }
                    const int bb = b + syncTone[i] * mult + shiftOf[i];
                    if (bb < 0 || bb >= sg.nBins) { ok = false; break; }
                    a += (double)sg.p[(size_t)f * (size_t)sg.nBins + (size_t)bb] * inv[(size_t)bb];
                }
                if (!ok) continue;
                const float sc = (float)(a / ns - 1.0);
                if (sc > best[(size_t)b]) { best[(size_t)b] = sc; bestLag[(size_t)b] = lag; }
            }
        }
    }
    if (wsjtDebug()) {
        std::vector<int> ord((size_t)nb);
        for (int b = 0; b < nb; b++) ord[(size_t)b] = b;
        std::partial_sort(ord.begin(), ord.begin() + std::min(nb, 4), ord.end(), [&](int x, int y) { return best[(size_t)x] > best[(size_t)y]; });
        for (int k = 0; k < std::min(nb, 4); k++) printf("  syncScan best: bin %d lag %d score %.2f\n", sg.binLo + ord[(size_t)k], bestLag[(size_t)ord[(size_t)k]], best[(size_t)ord[(size_t)k]]);
    }
    // against broadband events (a noise crash, a click raises every bin at one time): a peak must stand out from the bins around it
    std::vector<float> local((size_t)nb, 0.f);
    {
        std::vector<float> win;
        for (int b = 0; b < nb; b++) {
            win.clear();
            for (int k = std::max(0, b - 25); k <= std::min(nb - 1, b + 25); k++) if (std::abs(k - b) > 4) win.push_back(best[(size_t)k]);
            if (win.empty()) continue;
            std::nth_element(win.begin(), win.begin() + (long)win.size() / 2, win.end());
            local[(size_t)b] = win[win.size() / 2];
        }
    }
    for (int b = 0; b < nb; b++) {
        bool pk = best[(size_t)b] >= minScore && (local[(size_t)b] < 3.0f || best[(size_t)b] >= 1.5f * local[(size_t)b] + 0.2f);
        for (int k = -hw; k <= hw && pk; k++) {
            const int c = b + k;
            if (k && c >= 0 && c < nb && (best[(size_t)c] > best[(size_t)b] || (best[(size_t)c] == best[(size_t)b] && k < 0))) pk = false;
        }
        if (pk) out.push_back({sg.binLo + b, bestLag[(size_t)b], best[(size_t)b]});
    }
    std::sort(out.begin(), out.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    if (out.size() > maxCand) out.resize(maxCand);
    return out;
}

double fineFit(const std::vector<float>& y, double rate, int N, const std::vector<int8_t>& syncTone, double toneStep, long& start, double& freq, double df) {
    auto energy = [&](long s0, double f) {
        double e = 0;
        for (size_t i = 0; i < syncTone.size(); i++)
            if (syncTone[i] >= 0) e += toneEnergy(y, rate, s0 + (long)i * N, N, f + syncTone[i] * toneStep);
        return e;
    };
    long bs = start;
    double bf = freq, be = -1;
    const long st1 = std::max(1, N / 16);
    for (int k = -2; k <= 2; k++)
        for (int q = -2; q <= 2; q++) {
            const double e = energy(start + k * st1, freq + q * df / 4);
            if (e > be) { be = e; bs = start + k * st1; bf = freq + q * df / 4; }
        }
    const long s2 = bs; const double f2 = bf;
    for (int k = -1; k <= 1; k++)
        for (int q = -1; q <= 1; q++) {
            if (!k && !q) continue;
            const double e = energy(s2 + k * (st1 / 2), f2 + q * df / 8);
            if (e > be) { be = e; bs = s2 + k * (st1 / 2); bf = f2 + q * df / 8; }
        }
    start = bs; freq = bf;
    return be;
}

void subtractTones(std::vector<float>& y, double rate, long start, int N, const std::vector<double>& freqs) {
    for (size_t i = 0; i < freqs.size(); i++) {
        const long s0 = start + (long)i * N;
        const long a = std::max(0L, s0), b = std::min((long)y.size(), s0 + N);
        if (b <= a) continue;
        const double w = 2 * M_PI * freqs[i] / rate;
        double cr = 0, ci = 0, cc = 0, ss = 0, cs = 0;
        for (long n = a; n < b; n++) {
            const double c = std::cos(w * (double)(n - s0)), sn = std::sin(w * (double)(n - s0)), v = y[(size_t)n];
            cr += v * c; ci += v * sn; cc += c * c; ss += sn * sn; cs += c * sn;
        }
        const double det = cc * ss - cs * cs;
        if (det < 1e-9) continue;
        const double A = (cr * ss - ci * cs) / det, B = (ci * cc - cr * cs) / det;
        for (long n = a; n < b; n++) y[(size_t)n] -= (float)(A * std::cos(w * (double)(n - s0)) + B * std::sin(w * (double)(n - s0)));
    }
}

bool wsjtDebug() { static const bool d = std::getenv("HFDIG_WSJT_DEBUG") != nullptr; return d; }

void fillCallGrid(FtxDecode& d) {
    ftx::callAndGrid(d.msg, d.call, d.grid);
    d.cq = d.msg.compare(0, 3, "CQ ") == 0;
}

// ---------------------------------------------------------------- 72-bit messages

namespace {

constexpr int NBASE = 37 * 36 * 10 * 27 * 27 * 27;   // 262177560
constexpr int NGBASE = 180 * 180;
const char kCallChars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ ";
const char kTextChars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?";

int nchar(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    return 36;
}

std::string upperCollapse(const std::string& s) {
    std::string o;
    bool sp = true;
    for (char c : s) {
        c = (char)std::toupper((unsigned char)c);
        if (c == ' ' || c == '\t') { if (!sp) o += ' '; sp = true; }
        else { o += c; sp = false; }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < s.size()) {
        const size_t j = s.find(' ', i);
        w.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return w;
}

// a callsign into 28 bits; -1 when it is not a standard call
long packCall(std::string call) {
    if (call == "CQ") return NBASE + 1;
    if (call == "QRZ") return NBASE + 2;
    if (call == "DE") return 267796945;
    if (call.compare(0, 4, "3DA0") == 0) call = "3D0" + call.substr(4);
    if (call.size() > 6) return -1;
    if (call.size() >= 3 && call.compare(0, 2, "3X") == 0 && call[2] >= 'A' && call[2] <= 'Z') call = "Q" + call.substr(2);
    call.resize(6, ' ');
    std::string tmp;
    if (call[2] >= '0' && call[2] <= '9') tmp = call;
    else if (call[1] >= '0' && call[1] <= '9') {
        if (call[5] != ' ') return -1;
        tmp = " " + call.substr(0, 5);
    } else return -1;
    for (char& c : tmp) c = (char)std::toupper((unsigned char)c);
    auto L = [](char c) { return (c >= 'A' && c <= 'Z') || c == ' '; };
    auto D = [](char c) { return c >= '0' && c <= '9'; };
    const bool ok = (L(tmp[0]) || D(tmp[0])) && (L(tmp[1]) && tmp[1] != ' ' ? true : D(tmp[1])) && D(tmp[2]) && L(tmp[3]) && L(tmp[4]) && L(tmp[5]);
    if (!ok) return -1;
    long n = nchar(tmp[0]);
    n = 36 * n + nchar(tmp[1]);
    n = 10 * n + nchar(tmp[2]);
    n = 27 * n + nchar(tmp[3]) - 10;
    n = 27 * n + nchar(tmp[4]) - 10;
    n = 27 * n + nchar(tmp[5]) - 10;
    return n;
}

std::string unpackCall(long n) {
    if (n >= NBASE) return std::string();
    std::string w(6, '.');
    long v = n;
    w[5] = kCallChars[v % 27 + 10]; v /= 27;
    w[4] = kCallChars[v % 27 + 10]; v /= 27;
    w[3] = kCallChars[v % 27 + 10]; v /= 27;
    w[2] = kCallChars[v % 10]; v /= 10;
    w[1] = kCallChars[v % 36]; v /= 36;
    w[0] = kCallChars[v];
    size_t i = 0;
    while (i < 4 && w[i] == ' ') i++;
    w = w.substr(i);
    while (!w.empty() && w.back() == ' ') w.pop_back();
    if (w.compare(0, 3, "3D0") == 0) w = "3DA0" + w.substr(3);
    if (w.size() >= 2 && w[0] == 'Q' && w[1] >= 'A' && w[1] <= 'Z') w = "3X" + w.substr(1);
    return w;
}

} // namespace

bool packGrid15(const std::string& g0, int& ng) {
    std::string g = g0;
    if (g.empty()) { ng = NGBASE + 1; return true; }
    auto num = [](const std::string& s, int& v) {
        if (s.empty() || s.size() > 3) return false;
        size_t i = 0;
        int sg = 1;
        if (s[0] == '-' || s[0] == '+') { sg = s[0] == '-' ? -1 : 1; i = 1; }
        if (i >= s.size()) return false;
        int r = 0;
        for (; i < s.size(); i++) { if (!isdigit((unsigned char)s[i])) return false; r = r * 10 + (s[i] - '0'); }
        v = sg * r;
        return true;
    };
    if (g[0] == '-') {
        int n;
        if (num(g, n) && -n >= 1 && -n <= 30) { ng = NGBASE + 1 - n; return true; }
    } else if (g.compare(0, 2, "R-") == 0) {
        int n;
        if (num(g.substr(1), n) && -n >= 1 && -n <= 30) { ng = NGBASE + 31 - n; return true; }
    } else if (g == "RO") { ng = NGBASE + 62; return true; }
    else if (g == "RRR") { ng = NGBASE + 63; return true; }
    else if (g == "73") { ng = NGBASE + 64; return true; }
    // other reports (+05, R+05, 0, -35 ...) travel as the "locators" KA00 .. KA99 / LA00 .. LA99
    int n = 99;
    bool r = false;
    if (num(g, n)) {}
    else if (g[0] == 'R' && num(g.substr(1), n)) r = true;
    else n = 99;
    char buf[8];
    if (n >= -50 && n <= 49) { snprintf(buf, sizeof buf, "%s%02d", r ? "LA" : "KA", n + 50); g = buf; }
    if (g.size() != 4) return false;
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3])) return false;
    const int x0 = 20 * (g[0] - 'A') + 2 * (g[2] - '0');                        // west edge, degrees east of -180
    const int y0 = 10 * (g[1] - 'A') + (g[3] - '0');                            // south edge, degrees north of -90
    const double dlong = 180.0 - ((double)x0 + 1.0 + 1.0 / 24.0);              // west longitude of the middle of the square (the subsquare "mm")
    const int lon = (int)dlong;                                                 // truncates toward zero, as Fortran int()
    ng = ((lon + 180) / 2) * 180 + y0;
    return true;
}

std::string unpackGrid15(int ng) {
    char b[16];
    if (ng < 32400) {
        const int y = ng % 180, a = ng / 180;
        const int dlong = a * 2 - 180 + 2;
        const int x = 180 - dlong;                                              // degrees east of -180 at the west edge of the square
        if (x < 0 || x >= 360) return std::string();
        const char g[5] = {(char)('A' + x / 20), (char)('A' + y / 10), (char)('0' + (x / 2) % 10), (char)('0' + y % 10), 0};
        if (g[0] == 'K' && g[1] == 'A') { snprintf(b, sizeof b, "%+03d", (g[2] - '0') * 10 + (g[3] - '0') - 50); return b; }
        if (g[0] == 'L' && g[1] == 'A') { snprintf(b, sizeof b, "R%+03d", (g[2] - '0') * 10 + (g[3] - '0') - 50); return b; }
        return g;
    }
    const int n = ng - NGBASE - 1;
    if (n >= 1 && n <= 30) { snprintf(b, sizeof b, "-%02d", n); return b; }
    if (n >= 31 && n <= 60) { snprintf(b, sizeof b, "R-%02d", n - 30); return b; }
    if (n == 61) return "RO";
    if (n == 62) return "RRR";
    if (n == 63) return "73";
    return std::string();
}

bool packJt72(const std::string& text, uint8_t* sym) {
    std::string msg = upperCollapse(text);
    if (msg.size() > 22) msg.resize(22);
    long nc1 = 0, nc2 = 0;
    int ng = 0;
    bool freeText = false;
    // CQ 000 .. 999, CQ DX and CQ aa are folded into a call, as WSJT-X does
    if (msg.compare(0, 3, "CQ ") == 0 && msg.size() >= 4 && isdigit((unsigned char)msg[3]) && (msg.size() == 4 || msg[4] == ' ')) msg = "CQ 00" + msg.substr(3);
    if (msg.compare(0, 6, "CQ DX ") == 0) msg[2] = '9';
    if (msg.size() >= 6 && msg.compare(0, 3, "CQ ") == 0 && msg[3] >= 'A' && msg[3] <= 'Z' && msg[4] >= 'A' && msg[4] <= 'Z' && msg[5] == ' ') msg = "E9" + msg.substr(3);
    auto w = words(msg);
    bool done = false;
    if (!w.empty() && w[0] == "CQ" && w.size() >= 3 && w[1].size() == 3 && isdigit((unsigned char)w[1][0]) && isdigit((unsigned char)w[1][1]) && isdigit((unsigned char)w[1][2])) {
        // "CQ 123 K1ABC FN42": the frequency in the first call
        nc1 = NBASE + 3 + atoi(w[1].c_str());
        nc2 = packCall(w[2]);
        std::string c3 = w.size() > 3 ? w[3] : "";
        if (nc2 >= 0 && w.size() <= 4 && packGrid15(c3, ng)) done = true;
    } else if (w.size() >= 2 && w.size() <= 4) {
        std::string c3 = w.size() >= 3 ? w[2] : "";
        if (w.size() == 4 && w[3] == "OOO") w.pop_back();
        else if (w.size() == 4) c3 = "~";
        nc1 = packCall(w[0]);
        nc2 = packCall(w[1]);
        if (nc1 >= 0 && nc2 >= 0 && nc2 < NBASE && packGrid15(c3, ng)) done = true;
    }
    if (!done) {
        freeText = true;
        std::string t = msg;
        t.resize(std::max<size_t>(13, 0), ' ');
        t = t.substr(0, 13);
        while (t.size() < 13) t += ' ';
        long a1 = 0, a2 = 0, a3 = 0;
        auto code = [](char c) {
            const char* p = std::strchr(kTextChars, c);
            return p && c ? (int)(p - kTextChars) : 36;
        };
        for (int i = 0; i < 5; i++) a1 = a1 * 42 + code(t[(size_t)i]);
        for (int i = 5; i < 10; i++) a2 = a2 * 42 + code(t[(size_t)i]);
        for (int i = 10; i < 13; i++) a3 = a3 * 42 + code(t[(size_t)i]);
        nc1 = a1 + a1; if (a3 & 32768) nc1++;
        nc2 = a2 + a2; if (a3 & 65536) nc2++;
        ng = (int)(a3 & 32767) + 32768;
    }
    (void)freeText;
    sym[0] = (uint8_t)((nc1 >> 22) & 63); sym[1] = (uint8_t)((nc1 >> 16) & 63); sym[2] = (uint8_t)((nc1 >> 10) & 63); sym[3] = (uint8_t)((nc1 >> 4) & 63);
    sym[4] = (uint8_t)(4 * (nc1 & 15) + ((nc2 >> 26) & 3));
    sym[5] = (uint8_t)((nc2 >> 20) & 63); sym[6] = (uint8_t)((nc2 >> 14) & 63); sym[7] = (uint8_t)((nc2 >> 8) & 63); sym[8] = (uint8_t)((nc2 >> 2) & 63);
    sym[9] = (uint8_t)(16 * (nc2 & 3) + ((ng >> 12) & 15));
    sym[10] = (uint8_t)((ng >> 6) & 63); sym[11] = (uint8_t)(ng & 63);
    return true;
}

std::string unpackJt72(const uint8_t* d, bool* freeTextOut) {
    const long nc1 = ((long)d[0] << 22) + ((long)d[1] << 16) + ((long)d[2] << 10) + ((long)d[3] << 4) + ((d[4] >> 2) & 15);
    const long nc2 = ((long)(d[4] & 3) << 26) + ((long)d[5] << 20) + ((long)d[6] << 14) + ((long)d[7] << 8) + ((long)d[8] << 2) + ((d[9] >> 4) & 3);
    const int ng = ((d[9] & 15) << 12) + (d[10] << 6) + d[11];
    if (freeTextOut) *freeTextOut = ng >= 32768;
    std::string msg;
    if (ng >= 32768) {
        long a1 = nc1, a2 = nc2, a3 = ng & 32767;
        if (a1 & 1) a3 += 32768;
        a1 /= 2;
        if (a2 & 1) a3 += 65536;
        a2 /= 2;
        std::string t(13, ' ');
        for (int i = 4; i >= 0; i--) { t[(size_t)i] = kTextChars[a1 % 42]; a1 /= 42; }
        for (int i = 9; i >= 5; i--) { t[(size_t)i] = kTextChars[a2 % 42]; a2 /= 42; }
        for (int i = 12; i >= 10; i--) { t[(size_t)i] = kTextChars[a3 % 42]; a3 /= 42; }
        while (!t.empty() && t.back() == ' ') t.pop_back();
        return t;
    }
    if (nc1 > 267796945 || nc2 >= NBASE) return std::string();     // prefix / suffix messages (iv2 > 0) and nonsense: not supported
    std::string c1, c2 = unpackCall(nc2);
    bool cqnnn = false;
    if (nc1 < NBASE) c1 = unpackCall(nc1);
    else if (nc1 == NBASE + 1) c1 = "CQ";
    else if (nc1 == NBASE + 2) c1 = "QRZ";
    else if (nc1 == 267796945) c1 = "DE";
    else {
        const long f = nc1 - NBASE - 3;
        if (f >= 0 && f <= 999) { char b[16]; snprintf(b, sizeof b, "CQ %03ld", f); c1 = b; cqnnn = true; }
        else return std::string();
    }
    if (c1.empty() || c2.empty()) return std::string();
    const std::string grid = unpackGrid15(ng);
    msg = c1 + " " + c2;
    if (!grid.empty()) msg += " " + grid;
    (void)cqnnn;
    if (msg.compare(0, 6, "CQ9DX ") == 0) msg[2] = ' ';
    if (msg.size() >= 5 && msg.compare(0, 2, "E9") == 0 && msg[2] >= 'A' && msg[2] <= 'Z' && msg[3] >= 'A' && msg[3] <= 'Z' && msg[4] == ' ') msg = "CQ " + msg.substr(2);
    if (msg.compare(0, 5, "CQ 00") == 0 && msg.size() > 5 && isdigit((unsigned char)msg[5])) msg = "CQ " + msg.substr(5);
    return msg;
}


// ---------------------------------------------------------------- LDPC

void ldpcEncode(const LdpcTab& t, const uint8_t* msg, uint8_t* cw) {
    const int m = t.n - t.k;
    for (int j = 0; j < t.k; j++) cw[j] = msg[j] & 1;
    for (int i = 0; i < m; i++) {
        const uint8_t* row = t.gen + (size_t)i * (size_t)t.kb;
        int sum = 0;
        for (int j = 0; j < t.k; j++) sum += ((row[j / 8] >> (7 - j % 8)) & 1) & msg[j];
        cw[t.k + i] = (uint8_t)(sum & 1);
    }
}

int ldpcBp(const LdpcTab& t, const float* llr, int iters, uint8_t* cw) {
    const int n = t.n, m = t.n - t.k, w = t.w;
    std::vector<float> tov((size_t)n * 3, 0.f), toc((size_t)m * (size_t)w, 0.f), zn((size_t)n);
    std::vector<float> L((size_t)n);
    for (int i = 0; i < n; i++) L[(size_t)i] = -llr[i];   // standard convention: positive = 0
    int ncheck = m;
    for (int it = 0; it <= iters; it++) {
        for (int i = 0; i < n; i++) zn[(size_t)i] = L[(size_t)i] + tov[(size_t)i * 3] + tov[(size_t)i * 3 + 1] + tov[(size_t)i * 3 + 2];
        for (int i = 0; i < n; i++) cw[i] = zn[(size_t)i] < 0 ? 1 : 0;
        ncheck = 0;
        for (int j = 0; j < m; j++) {
            int sy = 0;
            for (int i = 0; i < t.nrw[j]; i++) sy ^= cw[t.nm[(size_t)j * (size_t)w + (size_t)i] - 1];
            ncheck += sy;
        }
        if (ncheck == 0 || it == iters) break;
        // bit -> check messages: the posterior without the check's own message
        for (int j = 0; j < m; j++)
            for (int i = 0; i < t.nrw[j]; i++) {
                const int b = t.nm[(size_t)j * (size_t)w + (size_t)i] - 1;
                float v = zn[(size_t)b];
                for (int k = 0; k < 3; k++) if (t.mn[(size_t)b * 3 + (size_t)k] - 1 == j) v -= tov[(size_t)b * 3 + (size_t)k];
                toc[(size_t)j * (size_t)w + (size_t)i] = std::tanh(0.5f * std::max(-30.f, std::min(30.f, v)));
            }
        // check -> bit messages
        for (int b = 0; b < n; b++)
            for (int k = 0; k < 3; k++) {
                const int j = t.mn[(size_t)b * 3 + (size_t)k] - 1;
                float prod = 1.f;
                for (int i = 0; i < t.nrw[j]; i++) {
                    if (t.nm[(size_t)j * (size_t)w + (size_t)i] - 1 == b) continue;
                    prod *= toc[(size_t)j * (size_t)w + (size_t)i];
                }
                prod = std::max(-0.999999f, std::min(0.999999f, prod));
                tov[(size_t)b * 3 + (size_t)k] = 2.f * std::atanh(prod);
            }
    }
    return ncheck;
}

bool ldpcOsd(const LdpcTab& t, const float* llr, int order, uint8_t* cw, bool (*check)(const uint8_t*, void*), void* ctx) {
    const int n = t.n, k = t.k, m = n - k;
    constexpr int W = 4;   // 256 bits
    typedef std::array<uint64_t, W> Row;
    auto getb = [](const Row& r, int i) { return (int)((r[(size_t)i / 64] >> (i % 64)) & 1); };
    auto flip = [](Row& r, int i) { r[(size_t)i / 64] ^= 1ull << (i % 64); };
    std::vector<Row> G((size_t)k, Row{});
    for (int j = 0; j < k; j++) {
        flip(G[(size_t)j], j);
        for (int i = 0; i < m; i++) if ((t.gen[(size_t)i * (size_t)t.kb + (size_t)(j / 8)] >> (7 - j % 8)) & 1) flip(G[(size_t)j], k + i);
    }
    std::vector<int> idx((size_t)n);
    for (int i = 0; i < n; i++) idx[(size_t)i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return std::fabs(llr[a]) > std::fabs(llr[b]); });
    // Gauss-Jordan on the columns in order of reliability
    std::vector<int> pivCol((size_t)k, -1);   // the column of the pivot of each row
    std::vector<char> used((size_t)k, 0);
    int found = 0;
    for (int pos = 0; pos < n && found < k; pos++) {
        const int c = idx[(size_t)pos];
        int r = -1;
        for (int i = 0; i < k; i++) if (!used[(size_t)i] && getb(G[(size_t)i], c)) { r = i; break; }
        if (r < 0) continue;
        used[(size_t)r] = 1; pivCol[(size_t)r] = c; found++;
        for (int i = 0; i < k; i++) if (i != r && getb(G[(size_t)i], c)) for (int q = 0; q < W; q++) G[(size_t)i][(size_t)q] ^= G[(size_t)r][(size_t)q];
    }
    if (found < k) return false;
    Row hard{};
    for (int i = 0; i < n; i++) if (llr[i] > 0) flip(hard, i);
    Row base{};
    for (int r = 0; r < k; r++) if (getb(hard, pivCol[(size_t)r])) for (int q = 0; q < W; q++) base[(size_t)q] ^= G[(size_t)r][(size_t)q];
    struct Cand { float d; int a, b; };
    std::vector<Cand> best;
    auto dist = [&](const Row& c) {
        float d = 0;
        for (int q = 0; q < W; q++) {
            uint64_t x = c[(size_t)q] ^ hard[(size_t)q];
            while (x) { const int b = __builtin_ctzll(x); x &= x - 1; const int i = q * 64 + b; if (i < n) d += std::fabs(llr[i]); }
        }
        return d;
    };
    auto consider = [&](float d, int a, int b) {
        if (best.size() < 24) { best.push_back({d, a, b}); if (best.size() == 24) std::sort(best.begin(), best.end(), [](const Cand& x, const Cand& y) { return x.d < y.d; }); }
        else if (d < best.back().d) {
            best.back() = {d, a, b};
            for (size_t i = best.size() - 1; i > 0 && best[i].d < best[i - 1].d; i--) std::swap(best[i], best[i - 1]);
        }
    };
    consider(dist(base), -1, -1);
    if (order >= 1)
        for (int a = 0; a < k; a++) {
            Row c = base;
            for (int q = 0; q < W; q++) c[(size_t)q] ^= G[(size_t)a][(size_t)q];
            consider(dist(c), a, -1);
            if (order >= 2)
                for (int b = a + 1; b < k; b++) {
                    Row c2 = c;
                    for (int q = 0; q < W; q++) c2[(size_t)q] ^= G[(size_t)b][(size_t)q];
                    consider(dist(c2), a, b);
                }
        }
    std::sort(best.begin(), best.end(), [](const Cand& x, const Cand& y) { return x.d < y.d; });
    for (const Cand& cd : best) {
        Row c = base;
        if (cd.a >= 0) for (int q = 0; q < W; q++) c[(size_t)q] ^= G[(size_t)cd.a][(size_t)q];
        if (cd.b >= 0) for (int q = 0; q < W; q++) c[(size_t)q] ^= G[(size_t)cd.b][(size_t)q];
        for (int i = 0; i < n; i++) cw[i] = (uint8_t)getb(c, i);
        if (check(cw, ctx)) return true;
    }
    return false;
}

} // namespace wsjt
} // namespace dect2
