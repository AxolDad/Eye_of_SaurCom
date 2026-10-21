// JT65 (A / B / C) of the HF digital receiver: 126 symbols of 4096 / 11025 s, 65-FSK (tone 0 sync, 2 .. 65 data) with spacing 2.692 Hz x 1 / 2 / 4,
// 60 s periods; 72-bit messages as 12 six-bit symbols of a Reed-Solomon (63,12) code over GF(64), 7 x 9 interleaving, Gray code.
// Decoder: a spectrogram at 11025 Hz (one symbol per FFT) -> the sync pattern (tone 0 at the 63 symbols where the pseudo-random vector is 1) over
// frequency and time -> a fine fit -> the 64 tone energies of every data symbol -> symbol probabilities -> Reed-Solomon decoding with
// erasures of the least reliable symbols (several erasure counts; the Berlekamp-Massey decoder handles errors and erasures together) ->
// the codeword closest to the received energies is taken, with a plausibility test against false decodes. (WSJT-X's Koetter-Vardy soft decoder
// and its "deep search" are not part of this; the hard-erasure decoder is about 2 dB less sensitive.)
#include "hfdig_wsjt_int.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace dect2 {
namespace wsjt {

namespace {

const uint8_t kNprc[126] = {1,0,0,1,1,0,0,0,1,1,1,1,1,1,0,1,0,1,0,0,
                            0,1,0,1,1,0,0,1,0,0,0,1,1,1,0,0,1,1,1,1,
                            0,1,1,0,1,1,1,1,0,0,0,1,1,0,1,0,1,0,1,1,
                            0,0,1,1,0,1,0,1,0,1,0,0,1,0,0,0,0,0,0,1,
                            1,0,0,0,0,0,0,0,1,1,0,1,0,0,1,0,1,1,0,1,
                            0,1,0,1,0,0,1,1,0,0,1,0,0,1,0,0,0,0,1,1,
                            1,1,1,1,1,1};

struct Gf {
    uint8_t ex[256], lg[64];
    Gf() {
        int x = 1;
        for (int i = 0; i < 63; i++) { ex[i] = (uint8_t)x; lg[x] = (uint8_t)i; x <<= 1; if (x & 64) x ^= 0x43; }
        for (int i = 63; i < 256; i++) ex[i] = ex[i - 63];
        lg[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return a && b ? ex[lg[a] + lg[b]] : 0; }
    uint8_t inv(uint8_t a) const { return ex[(63 - lg[a]) % 63]; }
    uint8_t pw(int e) const { e %= 63; if (e < 0) e += 63; return ex[e]; }
};
const Gf& gf() { static const Gf g; return g; }

constexpr int kFcr = 3, kNroots = 51;

const std::vector<uint8_t>& genPoly() {   // g[k]: coefficient of x^k, k = 0 .. 51
    static const std::vector<uint8_t> g = [] {
        const Gf& f = gf();
        std::vector<uint8_t> p(1, 1);
        for (int j = 0; j < kNroots; j++) {
            const uint8_t r = f.pw(kFcr + j);
            std::vector<uint8_t> q(p.size() + 1, 0);
            for (size_t i = 0; i < p.size(); i++) { q[i + 1] ^= p[i]; q[i] ^= f.mul(p[i], r); }
            p = q;
        }
        return p;
    }();
    return g;
}

constexpr double kRate = 11025.0, kSym = 4096.0 / 11025.0;
const int kN = 4096, kHop = 1024;

int igrayInv(int g) { int n = g; for (int sh = 1; (n >> sh) > 0; sh <<= 1) n ^= n >> sh; return n; }   // WSJT-X igray(n, -1)

void interleave63(const uint8_t* in, uint8_t* out) {   // idir = 1: out(j + 9 i) = in(i + 7 j)
    for (int i = 0; i < 7; i++) for (int j = 0; j < 9; j++) out[j + 9 * i] = in[i + 7 * j];
}

} // namespace

void rsEncode63(const uint8_t* dgen, uint8_t* sent) {
    const Gf& f = gf();
    const auto& g = genPoly();
    uint8_t reg[kNroots] = {};
    for (int i = 0; i < 12; i++) {   // data symbols, highest power first: dat1[i] = dgen[11 - i]
        const uint8_t fb = (uint8_t)(dgen[11 - i] ^ reg[0]);
        for (int j = 0; j < kNroots - 1; j++) reg[j] = (uint8_t)(reg[j + 1] ^ f.mul(fb, g[(size_t)(kNroots - 1 - j)]));
        reg[kNroots - 1] = f.mul(fb, g[0]);
    }
    for (int i = 0; i < kNroots; i++) sent[kNroots - 1 - i] = reg[i];
    for (int i = 0; i < 12; i++) sent[i + kNroots] = dgen[i];
}

int rsDecode63(uint8_t* cw, const int* era, int ne) {
    const Gf& f = gf();
    if (ne > kNroots) return -1;
    int S[kNroots];
    bool zero = true;
    for (int j = 0; j < kNroots; j++) {
        const uint8_t a = f.pw(kFcr + j);
        uint8_t s = 0;
        for (int i = 0; i < 63; i++) s = (uint8_t)(f.mul(s, a) ^ cw[i]);
        S[j] = s;
        if (s) zero = false;
    }
    if (zero) return 0;
    // erasure locator
    std::vector<uint8_t> gam(1, 1);
    for (int e = 0; e < ne; e++) {
        const uint8_t X = f.pw(62 - era[e]);
        std::vector<uint8_t> q(gam.size() + 1, 0);
        for (size_t i = 0; i < gam.size(); i++) { q[i] ^= gam[i]; q[i + 1] ^= f.mul(gam[i], X); }
        gam = q;
    }
    // modified syndromes T = gam * S mod x^51
    std::vector<uint8_t> T(kNroots, 0);
    for (int j = 0; j < kNroots; j++) {
        uint8_t t = 0;
        for (int i = 0; i <= ne && i <= j; i++) t ^= f.mul(gam[(size_t)i], (uint8_t)S[j - i]);
        T[(size_t)j] = t;
    }
    // Berlekamp-Massey on T[ne .. 50]
    const int nn = kNroots - ne;
    std::vector<uint8_t> C(1, 1), B(1, 1);
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int n = 0; n < nn; n++) {
        uint8_t d = T[(size_t)(ne + n)];
        for (int i = 1; i <= L && i < (int)C.size(); i++) d ^= f.mul(C[(size_t)i], T[(size_t)(ne + n - i)]);
        if (!d) { m++; continue; }
        const uint8_t coef = f.mul(d, f.inv(b));
        std::vector<uint8_t> Cn = C;
        if (Cn.size() < B.size() + (size_t)m) Cn.resize(B.size() + (size_t)m, 0);
        for (size_t i = 0; i < B.size(); i++) Cn[i + (size_t)m] ^= f.mul(coef, B[i]);
        if (2 * L <= n) { B = C; L = n + 1 - L; b = d; m = 1; }
        else m++;
        C = Cn;
    }
    if (ne + 2 * L > kNroots) return -1;
    // total locator
    std::vector<uint8_t> lam(gam.size() + C.size() - 1, 0);
    for (size_t i = 0; i < gam.size(); i++) for (size_t j = 0; j < C.size(); j++) lam[i + j] ^= f.mul(gam[i], C[j]);
    int deg = (int)lam.size() - 1;
    while (deg > 0 && lam[(size_t)deg] == 0) deg--;
    if (deg != ne + L) return -1;
    // Chien search
    int pos[64], np = 0;
    for (int p = 0; p < 63; p++) {
        const int xinvExp = -(62 - p);   // X^-1 = alpha^-(62-p)
        uint8_t v = 0;
        for (int i = 0; i <= deg; i++) v ^= f.mul(lam[(size_t)i], f.pw(xinvExp * i));
        if (v == 0) { if (np >= 63) return -1; pos[np++] = p; }
    }
    if (np != deg) return -1;
    // evaluator omega = S * lam mod x^51
    std::vector<uint8_t> om(kNroots, 0);
    for (int i = 0; i < kNroots; i++) {
        uint8_t t = 0;
        for (int j = 0; j <= deg && j <= i; j++) t ^= f.mul(lam[(size_t)j], (uint8_t)S[i - j]);
        om[(size_t)i] = t;
    }
    uint8_t out[63];
    std::memcpy(out, cw, 63);
    for (int k = 0; k < np; k++) {
        const int p = pos[k];
        const int xe = 62 - p;
        uint8_t num = 0;
        for (int i = 0; i < kNroots; i++) num ^= f.mul(om[(size_t)i], f.pw(-xe * i));
        uint8_t den = 0;
        for (int i = 1; i <= deg; i += 2) den ^= f.mul(lam[(size_t)i], f.pw(-xe * (i - 1)));
        if (!den) return -1;
        const uint8_t e = f.mul(f.mul(num, f.pw(xe * (1 - kFcr))), f.inv(den));
        out[p] ^= e;
    }
    // the result must be a codeword
    for (int j = 0; j < kNroots; j++) {
        const uint8_t a = f.pw(kFcr + j);
        uint8_t s = 0;
        for (int i = 0; i < 63; i++) s = (uint8_t)(f.mul(s, a) ^ out[i]);
        if (s) return -1;
    }
    int changed = 0;
    for (int i = 0; i < 63; i++) if (out[i] != cw[i]) changed++;
    std::memcpy(cw, out, 63);
    return changed;
}

std::vector<int> jt65Tones(const std::string& msg) {
    uint8_t dgen[12], sent[63], tx[63];
    if (!packJt72(msg, dgen)) return {};
    rsEncode63(dgen, sent);
    interleave63(sent, tx);
    std::vector<int> tones(126, 0);
    int k = 0;
    for (int j = 0; j < 126; j++)
        if (kNprc[j] == 0) { const int s = tx[k++]; tones[(size_t)j] = (s ^ (s >> 1)) + 2; }
    return tones;
}

void addJt65(const std::vector<int>& tones, int sub, double f0, double amp, double /*rate*/ rate, double start, std::vector<float>& out) {
    const double step = kRate / kN * (double)(1 << std::max(0, std::min(2, sub)));
    std::vector<double> fr(tones.size());
    for (size_t i = 0; i < tones.size(); i++) fr[i] = f0 + tones[i] * step;
    addFsk(fr, kSym, amp, rate, start, out);
}

namespace {

class Jt65 {
public:
    explicit Jt65(const Slot& s) : s_(s), mult_(1 << std::max(0, std::min(2, s.sub))), df_(kRate / kN) {}

    std::vector<FtxDecode> run() {
        std::vector<FtxDecode> res;
        y_ = resample8k(s_.x, s_.n, kRate);
        const double t0 = 1.0;
        const int binLo = std::max(2, (int)std::floor(s_.fLo / df_));
        const int fMaxBin = (int)std::ceil(s_.fHi / df_);
        const int binHi = std::min(kN / 2 - 2, fMaxBin + 66 * mult_ + 2);
        std::vector<int8_t> st(126, -1);
        for (int i = 0; i < 126; i++) if (kNprc[i]) st[(size_t)i] = 0;
        std::set<std::string> seen;
        for (int pass = 0; pass < 3; pass++) {
            sg_ = spectrogram(y_, kN, kHop, binLo, binHi, true);
            if (sg_.frames < 4 * 125 + 1) return res;
            const int lagLo = std::max(0, (int)std::floor((t0 + s_.dtMin - s_.x0) * kRate / kHop));
            const int lagHi = std::min(sg_.frames - 1 - 4 * 125, (int)std::ceil((t0 + s_.dtMax - s_.x0) * kRate / kHop));
            if (lagHi < lagLo) return res;
            const int nb = std::min(sg_.nBins, fMaxBin - binLo + 1);
            const std::vector<Cand> cands = syncScan(sg_, st, mult_, 4, lagLo, lagHi, nb, kMinSync, kMaxCand, 3);
            if (wsjtDebug()) printf("JT65 pass %d: %d frames, lags %d..%d, %d candidates\n", pass, sg_.frames, lagLo, lagHi, (int)cands.size());
            std::vector<std::pair<double, double>> taken;   // the frequency spans of what was decoded in this pass
            bool found = false;
            for (const Cand& c : cands) {
                const double f = c.bin * df_;
                bool skip = false;
                for (const auto& sp : taken) if (f > sp.first - 3 * df_ && f < sp.second + 3 * df_) skip = true;
                if (skip) continue;
                FtxDecode d;
                const bool ok = tryCandidate(c, t0, d);
                if (wsjtDebug()) printf("  cand bin %d (%.1f Hz) lag %d score %.2f: %s ne=%d avgP=%.3f cost=%.1f\n", c.bin, f, c.lag, c.score, ok ? "ok" : "no", lastNe, lastAvgP, lastCost);
                if (!ok) continue;
                // take the signal out of the audio: the weaker ones under and beside it come next
                const auto tn = jt65Tones(d.msg);
                std::vector<double> fr(tn.size());
                for (size_t i = 0; i < tn.size(); i++) fr[i] = lastFreq + tn[i] * df_ * mult_;
                subtractTones(y_, kRate, lastStart, kN, fr);
                taken.push_back({lastFreq, lastFreq + 66 * mult_ * df_});
                found = true;
                if (!seen.insert(d.msg).second) continue;
                d.slotUtc = s_.slotUtc;
                d.pass = pass;
                res.push_back(d);
            }
            if (!found) break;
        }
        return res;
    }

    static constexpr float kMinSync = 0.7f;
    static constexpr size_t kMaxCand = 16;
    static constexpr int kTrials = 500;

private:
    bool tryCandidate(const Cand& c, double t0, FtxDecode& out) {
        std::vector<int8_t> st(126, -1);
        for (int i = 0; i < 126; i++) if (kNprc[i]) st[(size_t)i] = 0;
        long bs = (long)c.lag * kHop;
        double bf = c.bin * df_;
        fineFit(y_, kRate, kN, st, df_ * mult_, bs, bf, df_);
        // Doppler drift: hypotheses of the total drift; the audio is warped to take it out
        const double eps = driftSearch(y_, kRate, kN, st, df_ * mult_, bs, bf, df_);
        std::vector<float> yw;
        if (eps != 0) {
            yw = warpDrift(y_, eps, (double)bs + 0.5 * 126 * kN, kRate);
            fineFit(yw, kRate, kN, st, df_ * mult_, bs, bf, df_);
        }
        const std::vector<float>& yy = eps != 0 ? yw : y_;
        lastEps = eps;
        // the 66 tones of each symbol (relative to the bin of tone 0)
        const int b0 = (int)std::lround(bf / df_);
        const double shift = bf - b0 * df_;
        const int nb = 66 * mult_ + 1;
        std::vector<std::vector<float>> pw(126, std::vector<float>((size_t)nb));
        for (int i = 0; i < 126; i++) symbolPowers(yy, kRate, bs + (long)i * kN, kN, shift, b0, nb, pw[(size_t)i].data());
        double es = 0, nn = 0;
        int ns = 0;
        for (int i = 0; i < 126; i++) {
            if (kNprc[i]) { es += pw[(size_t)i][0]; ns++; }
            nn += pw[(size_t)i][(size_t)mult_];
        }
        es /= ns; nn /= 126;
        if (nn <= 0) return false;
        const double sig = std::max(es - nn, 0.05 * nn);
        // probabilities of the 64 symbols of every data position, in codeword order
        double q[63][64];
        uint8_t hard[63];
        double rel[63];
        int k = 0;
        for (int i = 0; i < 126; i++) {
            if (kNprc[i]) continue;
            const int tp = k++;
            double l[64], mx = -1e300;
            for (int g = 0; g < 64; g++) {
                const double e = pw[(size_t)i][(size_t)((g + 2) * mult_)];
                l[g] = logI0(2 * std::sqrt(sig * e) / nn);
                mx = std::max(mx, l[g]);
            }
            double tot = 0;
            for (int g = 0; g < 64; g++) { l[g] = std::exp(l[g] - mx); tot += l[g]; }
            const int ii = tp / 9, jj = tp % 9;
            const int cw = 62 - (ii + 7 * jj);
            for (int g = 0; g < 64; g++) q[cw][igrayInv(g)] = std::max(l[g] / tot, 1e-12);
            if (wsjtDebug() && c.score > 5 && tp < 24) { int am = 0; for (int g = 1; g < 64; g++) if (pw[(size_t)i][(size_t)((g + 2) * mult_)] > pw[(size_t)i][(size_t)((am + 2) * mult_)]) am = g; printf("    sym %d tone %d\n", i, am + 2); }
            int bv = 0;
            for (int v = 1; v < 64; v++) if (q[cw][v] > q[cw][bv]) bv = v;
            hard[cw] = (uint8_t)bv;
            rel[cw] = q[cw][bv];
        }
        int order[63];
        for (int i = 0; i < 63; i++) order[i] = i;
        std::sort(order, order + 63, [&](int a, int b) { return rel[a] < rel[b]; });
        double bestCost = 1e300;
        uint8_t bestCw[63];
        int bestNe = -1;
        auto attempt = [&](int ne, const int* era) {
            uint8_t cw[63];
            std::memcpy(cw, hard, 63);
            if (rsDecode63(cw, era, ne) < 0) return false;
            double cost = 0;
            for (int i = 0; i < 63; i++) cost += std::log(q[i][hard[i]]) - std::log(q[i][cw[i]]);
            if (cost < bestCost) { bestCost = cost; std::memcpy(bestCw, cw, 63); bestNe = ne; }
            return true;
        };
        for (int ne = 0; ne <= 48; ne += 3) {
            int era[64];
            for (int i = 0; i < ne; i++) era[i] = order[i];
            attempt(ne, era);
        }
        if (bestNe < 0 || bestNe > 30) {
            // random erasure sets drawn by the reliabilities (the Franke-Taylor way): the least reliable symbols are erased most often
            uint64_t rs = 0x9E3779B97F4A7C15ull;
            auto rnd = [&]() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return ((rs >> 11) + 0.5) / 9007199254740992.0; };
            for (int trial = 0; trial < kTrials; trial++) {
                const int ne = 22 + (int)(rnd() * 15);
                double key[63];
                int idx[63];
                for (int i = 0; i < 63; i++) { key[i] = -std::log(rnd()) / (1.02 - rel[i]); idx[i] = i; }
                std::partial_sort(idx, idx + ne, idx + 63, [&](int a, int b) { return key[a] < key[b]; });
                int era[64];
                for (int i = 0; i < ne; i++) era[i] = idx[i];
                attempt(ne, era);
            }
        }
        if (bestNe < 0) return false;
        uint8_t sent[63];
        for (int i = 0; i < 63; i++) sent[62 - i] = bestCw[i];
        bool freeText = false;
        const std::string msg = unpackJt72(sent + 51, &freeText);
        if (msg.empty()) return false;
        // plausibility: how much of the energy the codeword explains
        double avgP = 0;
        for (int i = 0; i < 63; i++) avgP += q[i][bestCw[i]];
        avgP /= 63;
        lastAvgP = avgP; lastCost = bestCost; lastNe = bestNe; lastStart = bs; lastFreq = bf;
        if (!acceptable(avgP, bestCost, bestNe, freeText)) return false;
        if (10 * std::log10(sig / nn * df_ / 2500.0) < -28.0) return false;   // below the limit of the code: a decode this weak is an alias of a stronger signal
        out.mode = ftx::kJt65;
        out.msg = msg;
        out.hz = (float)bf;
        out.dt = (float)((double)bs / kRate + s_.x0 - t0);
        out.snrDb = (float)(10 * std::log10(sig / nn * df_ / 2500.0));
        fillCallGrid(out);
        return true;
    }

    // thresholds from the measurements of tests/test_hfdig_wsjt.cpp (noise alone, weak and strong signals)
    static bool acceptable(double avgP, double cost, int ne, bool freeText) {
        if (freeText) return ne == 0 && avgP > 0.35;
        (void)cost;
        return avgP > 0.12 || ne == 0;
    }

    Slot s_;
    int mult_;
    double df_;
    std::vector<float> y_;
    Spectrogram sg_;
public:
    double lastAvgP = 0, lastCost = 0, lastFreq = 0, lastEps = 0;
    long lastStart = 0;
    int lastNe = 0;
};

} // namespace

std::vector<FtxDecode> decodeJt65(const Slot& s) { Jt65 d(s); return d.run(); }

} // namespace wsjt
} // namespace dect2
