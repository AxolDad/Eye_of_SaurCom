// MFSK (hfdig_kbd.h): the modes of fldigi's src/mfsk/mfsk.cxx without the picture mode.
//   tones     numtones tones tonespacing = fs / symlen apart (fs 8000, MFSK11 and MFSK22 11025 Hz), centred on the audio frequency
//   symbols   symbits bits Gray coded onto the tone number; the K = 7 convolutional code (polynomials 0x6d and 0x4f) gives two bits per bit,
//             a diagonal bit interleaver of symbits x symbits x depth, the IZ8BLY varicode (codes start with 1 and end with 00)
//   receiver  a sliding DFT bank at four sub-bin offsets (a tuning error of a quarter bin or less costs nothing), the symbol clock from the
//             tone peak at 16 hop phases per symbol, soft symbols from the tone amplitudes, the de-interleaver, the Viterbi decoder
// Not here: the CWI avoidance of fldigi (the steady tones of interference are not punctured), the picture mode.
#include "hfdig_kbd_dsp.h"
#include "hfdig_kbd_tables.h"

namespace dect2 {
namespace kbd {
namespace {

struct MfskP {
    int fs, symlen, symbits, depth, tones, preamble;
};
const MfskP kMfsk[8] = {
    {8000, 2048, 5, 5, 32, 107},    // MFSK4
    {8000, 1024, 5, 5, 32, 107},    // MFSK8
    {11025, 1024, 4, 10, 16, 107},  // MFSK11
    {8000, 512, 4, 10, 16, 107},    // MFSK16
    {11025, 512, 4, 10, 16, 107},   // MFSK22
    {8000, 256, 3, 10, 8, 107},     // MFSK31
    {8000, 256, 4, 10, 16, 107},    // MFSK32
    {8000, 128, 4, 10, 16, 180},    // MFSK64
};
constexpr unsigned kPoly1 = 0x6d, kPoly2 = 0x4f;

}  // namespace
int mfskVaridec(unsigned code) {   // the character of a varicode word (leading 1 included), -1 when none
    static const std::vector<short> tab = [] {
        std::vector<short> t(4096, -1);
        for (int c = 0; c < 256; c++) {
            unsigned v = 0;
            for (const char* q = kMfskVaricode[c]; *q; q++) v = (v << 1) | (unsigned)(*q - '0');
            if (v < 4096) t[v] = (short)c;
        }
        return t;
    }();
    return code < 4096 ? tab[code] : -1;
}
namespace {

// ------------------------------------------------------------------ transmitter
class MfskMod : public Modulator {
public:
    MfskMod(const KbdConfig& c, double centre) : p_(kMfsk[std::clamp(c.variant, 0, 7)]), enc_(7, kPoly1, kPoly2), inl_(p_.symbits, p_.depth, true) {
        spacing_ = (double)p_.fs / p_.symlen;
        f0_ = centre - (p_.tones - 1) * spacing_ / 2.0;
        if (p_.fs != 8000) rs_ = std::make_unique<Resampler>(p_.fs, 8000);
        // clearbits(): the interleaver is filled with the code of zero bits, nothing is sent
        const int data = enc_.encode(0);
        for (int k = 0; k < p_.preamble; k++)
            for (int i = 0; i < 2; i++) {
                bitsh_ = (bitsh_ << 1) | (unsigned)((data >> i) & 1);
                if (++bitstate_ == p_.symbits) { inl_.bits(&bitsh_); bitstate_ = 0; bitsh_ = 0; }
            }
        enc_.reset();
        preBits_ = p_.preamble / 3;
        started_ = false;
    }
    void next(std::vector<float>& out, TextSrc& src) override {
        // send bits until one symbol has gone out
        const uint64_t before = symbols_;
        while (symbols_ == before) {
            if (preBits_ > 0) { sendBit(0); preBits_--; continue; }
            if (queue_.empty()) {
                if (!started_) { started_ = true; for (int c : {13, 2, 13}) queueChar(c); }
                else queueChar(src.next());
            }
            sendBit(queue_.front() - '0');
            queue_.erase(queue_.begin());
        }
        if (rs_) rs_->process(buf_.data(), buf_.size(), out); else out.insert(out.end(), buf_.begin(), buf_.end());
        buf_.clear();
    }
private:
    void queueChar(int c) { queue_ += kMfskVaricode[c & 255]; }
    void sendBit(int bit) {
        const int data = enc_.encode(bit);
        for (int i = 0; i < 2; i++) {
            bitsh_ = (bitsh_ << 1) | (unsigned)((data >> i) & 1);
            if (++bitstate_ == p_.symbits) {
                inl_.bits(&bitsh_);
                sendSymbol((int)bitsh_);
                bitstate_ = 0;
                bitsh_ = 0;
            }
        }
    }
    void sendSymbol(int sym) {
        sym = (int)grayCode((unsigned)(sym & (p_.tones - 1)));
        const double w = 2 * kPi * (f0_ + sym * spacing_) / p_.fs;
        for (int i = 0; i < p_.symlen; i++) {
            buf_.push_back((float)std::cos(ph_));
            ph_ += w;
            if (ph_ > 2 * kPi) ph_ -= 2 * kPi;
        }
        symbols_++;
    }
    MfskP p_;
    ConvEnc enc_;
    Interleaver inl_;
    std::unique_ptr<Resampler> rs_;
    double spacing_, f0_, ph_ = 0;
    unsigned bitsh_ = 0;
    int bitstate_ = 0, preBits_ = 0;
    bool started_ = false;
    uint64_t symbols_ = 0;
    std::string queue_;
    std::vector<float> buf_;
};

// ------------------------------------------------------------------ receiver
class MfskEngine : public Engine {
public:
    explicit MfskEngine(const KbdConfig& c) : cfg_(c), p_(kMfsk[std::clamp(c.variant, 0, 7)]) {
        spacing_ = (double)p_.fs / p_.symlen;
        hop_ = std::max(1, p_.symlen / kHops);
        if (p_.fs != 8000) rs_ = std::make_unique<Resampler>(8000, p_.fs);
        setCentre(c.centreHz);
    }
    double bandwidth() const override { return (p_.tones - 1) * spacing_; }
    void setCentre(double hz) override { centre_ = hz; build(); }
    void reset() override { build(); }

    void feed(const float* x, size_t n) override {
        if (rs_) { tmp_.clear(); rs_->process(x, n, tmp_); for (float v : tmp_) sample(v); }
        else for (size_t i = 0; i < n; i++) sample(x[i]);
    }

private:
    static constexpr uint64_t kSettle = 64;
    static constexpr int kHops = 16, kPaths = 4, kMargin = 3;
    void build() {
        const double f0 = centre_ - bandwidth() / 2.0;
        const int nt = p_.tones + 2 * kMargin;
        std::vector<double> f;
        for (int t = -kMargin; t < p_.tones + kMargin; t++)
            for (int k = 0; k < kPaths; k++) f.push_back(f0 + (t + (double)k / kPaths) * spacing_);
        (void)nt;
        bank_.init(p_.symlen, p_.fs, f);
        J_ = (int)f.size();
        nAlign_ = 2 * kMargin * kPaths + kPaths;
        align_ = kMargin * kPaths;
        r_.assign(nAlign_, 0.f);
        e_.assign(J_, 0.f);
        ph_.assign(kHops, 0.f);
        phN_.assign(kHops, 0);
        count_ = 0; hopIdx_ = 0; nextDec_ = kHops; decisions_ = 0;
        inl_ = std::make_unique<Interleaver>(p_.symbits, p_.depth, false);
        dec1_ = std::make_unique<Viterbi>(7, kPoly1, kPoly2, 45);
        dec2_ = std::make_unique<Viterbi>(7, kPoly1, kPoly2, 45);
        pair_[0] = pair_[1] = 0;
        hist_.clear(); wasLocked_ = false; symcounter_ = 0; met1_ = met2_ = 0; metric_ = 0; shreg_ = 1;
        locked = false; snrDb = 0; offsetHz = 0;
        tf_ = TextFilter();
        shown_ = 0;
    }
    void sample(float v) {
        bank_.feed(v);
        if (++count_ >= hop_) { count_ = 0; hop(); }
    }
    // peakness of the tones of alignment `a`
    float peakness(int a, double* sumOut = nullptr) const {
        double mx = 0, sum = 0;
        for (int i = 0; i < p_.tones; i++) { const double m = bank_.mag(a + i * kPaths); mx = std::max(mx, m); sum += m; }
        if (sumOut) *sumOut = sum;
        return sum > 1e-12 ? (float)(mx / sum) : 0.f;
    }
    void hop() {
        hopIdx_++;
        const int phase = (int)(hopIdx_ % kHops);
        // the clock averages start fast (1/n of the first visits) so that the symbol phase is right within a few symbols, not ten
        phN_[phase] = std::min(phN_[phase] + 1, 10);
        ph_[phase] += (1.f / (float)phN_[phase]) * (peakness(align_) - ph_[phase]);
        if (hopIdx_ < nextDec_) return;
        decide();
        // symbol clock: one hop per symbol towards the best phase
        int best = 0;
        for (int i = 1; i < kHops; i++) if (ph_[i] > ph_[best]) best = i;
        const int cur = (int)(nextDec_ % kHops);
        int d = best - cur;
        if (d > kHops / 2) d -= kHops;
        if (d < -kHops / 2) d += kHops;
        int adj = 0;
        if (d != 0 && ph_[best] > ph_[cur] * 1.03f + 1e-4f) adj = d > 0 ? 1 : -1;
        nextDec_ += kHops + adj;
    }
    void decide() {
        decisions_++;
        const int nt = p_.tones;
        double m[64];
        double sum = 0, mx = 0;
        int hard = 0;
        for (int i = 0; i < nt; i++) {
            m[i] = bank_.mag(align_ + i * kPaths);
            sum += m[i];
            if (m[i] > mx) { mx = m[i]; hard = i; }
        }
        // alignment statistics
        for (int a = 0; a < nAlign_; a++) r_[a] += (peakness(a) - r_[a]) * std::max(1.f / 16, 1.f / (float)decisions_);
        for (int j = 0; j < J_; j++) e_[j] += (float)(bank_.energy(j) - e_[j]) * std::max(1.f / 32, 1.f / (float)decisions_);
        if ((decisions_ & 7) == 0) realign();
        if (sum < 1e-9) return;
        const double avg = sum / nt;
        int burst = 0;
        for (int i = 0; i < nt; i++) if (m[i] > 2.0 * avg) burst++;
        const bool staticBurst = burst == nt;
        (void)hard;
        // soft symbols: Gray decoded tone amplitudes onto the bits
        double b[8] = {0};
        for (int i = 0; i < nt; i++) {
            const unsigned j = binaryCode((unsigned)i);
            for (int k = 0; k < p_.symbits; k++) b[k] += (j & (1u << (p_.symbits - k - 1))) ? m[i] : -m[i];
        }
        uint8_t sym[8];
        for (int k = 0; k < p_.symbits; k++) {
            sym[k] = staticBurst ? 128 : (uint8_t)std::clamp(128.0 + b[k] / sum * 256.0, 0.0, 255.0);
        }
        inl_->symbols(sym);
        for (int k = 0; k < p_.symbits; k++) decodeSymbol(sym[k]);
        // lock and level
        snrDb = (float)(20.0 * std::log10(std::max(1e-6, mx / std::max(1e-9, (sum - mx) / (nt - 1)))));
    }
    void realign() {
        // fractional offset: the best of the kPaths positions summed over the integer shifts; integer shift: the tones in use show up as
        // energy inside the window and not just outside it
        int bf = 0;
        float bs = -1;
        for (int f = 0; f < kPaths; f++) {
            float s = 0;
            for (int g = 0; g <= 2 * kMargin; g++) s += r_[g * kPaths + f];
            if (s > bs) { bs = s; bf = f; }
        }
        auto score = [&](int a) {
            double in = 0, out = 0;
            for (int i = 0; i < p_.tones; i++) in += e_[a + i * kPaths];
            int no = 0;
            for (int k : {-2, -1, p_.tones, p_.tones + 1}) { const int j = a + k * kPaths; if (j >= 0 && j < J_) { out += e_[j]; no++; } }
            return in / p_.tones - (no ? out / no : 0.0);
        };
        int bg = align_ / kPaths;
        double best = score(bg * kPaths + bf);
        // the integer shift is judged only after the preamble: its all-zero data is a single tone, which fits a window in several places
        // (the receiver moved to a wrong shift at once and garbled the first characters of MFSK32)
        if (decisions_ >= kSettle)
            for (int g = 0; g <= 2 * kMargin; g++) { const double s = score(g * kPaths + bf); if (s > best * 1.05 + 1e-12) { best = s; bg = g; } }
        align_ = bg * kPaths + bf;
        offsetHz = ((align_ - kMargin * kPaths) / (double)kPaths) * spacing_;
    }
    void decodeSymbol(uint8_t s) {
        pair_[0] = pair_[1];
        pair_[1] = s;
        symcounter_ = symcounter_ ? 0 : 1;
        int met = 0, bit;
        if (p_.symbits == 5 || p_.symbits == 3) {
            if (symcounter_) {
                bit = dec1_->decode(pair_, &met);
                met1_ = decayAvg(met1_, met, 50);
                if (met1_ < met2_) return;
                metric_ = met1_;
            } else {
                bit = dec2_->decode(pair_, &met);
                met2_ = decayAvg(met2_, met, 50);
                if (met2_ < met1_) return;
                metric_ = met2_;
            }
        } else {
            if (symcounter_) return;
            bit = dec2_->decode(pair_, &met);
            met2_ = decayAvg(met2_, met, 50);
            metric_ = met2_;
        }
        const double shown = std::clamp((metric_ - 60.0) * 0.5, 0.0, 100.0);
        shown_ = (float)shown;
        locked = shown >= kLockLevel;
        // the Viterbi metric rises slowly, so the bits just before the lock are already good: they are replayed through the varicode at the
        // lock (otherwise the first characters of a transmission are lost or garbled, the varicode starting in the middle of a character)
        if (!locked) {
            wasLocked_ = false;
            shreg_ = 1;
            hist_.push_back((uint8_t)bit);
            if (hist_.size() > kBack) hist_.erase(hist_.begin());
            return;
        }
        if (!wasLocked_) {
            wasLocked_ = true;
            shreg_ = 1;
            for (uint8_t hb : hist_) varBit(hb);
            hist_.clear();
        }
        varBit(bit);
    }

    void varBit(int bit) {
        shreg_ = (shreg_ << 1) | (unsigned)bit;
        if ((shreg_ & 7) == 1) {
            const int c = mfskVaridec(shreg_ >> 1);
            if (c > 0) tf_.put(out, c);
            shreg_ = 1;
        }
    }
    static constexpr size_t kBack = 64;
    std::vector<uint8_t> hist_;
    bool wasLocked_ = false;
    static constexpr float kLockLevel = 20.f;
    KbdConfig cfg_;
    MfskP p_;
    double spacing_, centre_ = 1500;
    int hop_;
    std::unique_ptr<Resampler> rs_;
    std::vector<float> tmp_;
    Bank bank_;
    int J_ = 0, nAlign_ = 0, align_ = 0, count_ = 0, symcounter_ = 0;
    uint64_t hopIdx_ = 0, nextDec_ = 0, decisions_ = 0;
    std::vector<float> r_, e_, ph_;
    std::vector<int> phN_;
    std::unique_ptr<Interleaver> inl_;
    std::unique_ptr<Viterbi> dec1_, dec2_;
    uint8_t pair_[2] = {0, 0};
    double met1_ = 0, met2_ = 0, metric_ = 0;
    unsigned shreg_ = 1;
    float shown_ = 0;
    TextFilter tf_;
};

} // namespace

std::unique_ptr<Engine> makeMfskEngine(const KbdConfig& c) { return std::make_unique<MfskEngine>(c); }
std::unique_ptr<Modulator> makeMfskMod(const KbdConfig& c, double centre) { return std::make_unique<MfskMod>(c, centre); }

} // namespace kbd
} // namespace dect2
