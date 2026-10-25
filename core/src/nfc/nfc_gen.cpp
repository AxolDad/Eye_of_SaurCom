// NFC test signal: see nfc_gen.h
#include "dect2/nfc/nfc_gen.h"
#include "dect2/nfc/nfc_proto.h"
#include "dect2/nfc/nfc_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace {

constexpr double kFc = 13.56e6;        // the field's carrier
constexpr int kBitFc = 128;            // one bit at 106 kbit/s, in carrier cycles
constexpr int kFdtCardFc = 1236;       // card after reader: (9 * 128 + 84) / fc, the anticollision commands' frame delay time
constexpr int kFdtReaderFc = 2400;     // reader after card (at least 1172 / fc)
constexpr int kBetweenFc = 40000;      // between the selection and the HLTA (about 3 ms)

struct Seg { double start, end; int kind; };   // in carrier cycles from the period's start; kind 1 pause (no carrier), 2 subcarrier on

bool oddParity(uint8_t b) { return (__builtin_popcount(b) & 1) == 0; }   // the parity bit: 1 when the byte has an even number of ones

std::vector<int> frameBits(const std::vector<uint8_t>& bytes, int bits) {
    std::vector<int> v;
    if (bits == 7) { for (int i = 0; i < 7; i++) v.push_back((bytes[0] >> i) & 1); return v; }
    for (uint8_t b : bytes) { for (int i = 0; i < 8; i++) v.push_back((b >> i) & 1); v.push_back(oddParity(b) ? 1 : 0); }
    return v;
}

// The reader: modified Miller. X = pause in the second half, Y = none, Z = pause at the start. 1 -> X; 0 -> Y, or Z after a 0 (and as the
// first bit); SOF = Z; EOF = a 0 then Y. Returns the time after the frame.
double miller(std::vector<Seg>& segs, double t, const std::vector<int>& bits, double pauseFc) {
    segs.push_back({t, t + pauseFc, 1});   // SOF: Z
    t += kBitFc;
    int prev = 0;
    for (int b : bits) {
        if (b) segs.push_back({t + kBitFc / 2, t + kBitFc / 2 + pauseFc, 1});
        else if (prev == 0) segs.push_back({t, t + pauseFc, 1});
        prev = b;
        t += kBitFc;
    }
    if (prev == 0) segs.push_back({t, t + pauseFc, 1});   // EOF: a logic 0 (Z after a 0, Y after a 1) ...
    t += kBitFc;
    t += kBitFc;                                          // ... then Y
    return t;
}

// The card: Manchester on the subcarrier. 1 = D (subcarrier in the first half), 0 = E (second half); SOF = D, EOF = F (none)
double manchester(std::vector<Seg>& segs, double t, const std::vector<int>& bits) {
    segs.push_back({t, t + kBitFc / 2, 2});   // SOF
    t += kBitFc;
    for (int b : bits) {
        if (b) segs.push_back({t, t + kBitFc / 2, 2}); else segs.push_back({t + kBitFc / 2, t + kBitFc, 2});
        t += kBitFc;
    }
    t += kBitFc;   // EOF
    return t;
}

std::vector<uint8_t> withCrc(std::vector<uint8_t> v) {
    const uint16_t c = nfc::crcA(v.data(), v.size());
    v.push_back((uint8_t)(c & 0xFF)); v.push_back((uint8_t)(c >> 8));
    return v;
}
std::vector<uint8_t> uidPart(const uint8_t* u, bool cascade) {
    std::vector<uint8_t> v;
    if (cascade) v.push_back(0x88);
    for (int i = 0; i < (cascade ? 3 : 4); i++) v.push_back(u[i]);
    v.push_back((uint8_t)(v[0] ^ v[1] ^ v[2] ^ v[3]));
    return v;
}

} // namespace

std::vector<uint8_t> nfcTestUid(int scene) {
    if (scene == 1) return {0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    return {0x04, 0x7B, 0xC2, 0x1A};
}

std::vector<NfcGenFrame> nfcTestExchange(int scene) {
    std::vector<NfcGenFrame> f;
    auto poll = [&](std::vector<uint8_t> b, int bits = 0) { f.push_back({true, std::move(b), bits}); };
    auto listen = [&](std::vector<uint8_t> b) { f.push_back({false, std::move(b), 0}); };
    const std::vector<uint8_t> uid = nfcTestUid(scene);
    if (scene == 1) {   // NTAG: 7-byte UID in two cascade levels, ATQA 44 00, SAK 04 then 00, then a READ of page 4
        poll({0x52}, 7);
        listen({0x44, 0x00});
        poll({0x93, 0x20});
        const auto cl1 = uidPart(uid.data(), true);
        listen(cl1);
        { std::vector<uint8_t> sel = {0x93, 0x70}; sel.insert(sel.end(), cl1.begin(), cl1.end()); poll(withCrc(sel)); }
        listen(withCrc({0x04}));
        poll({0x95, 0x20});
        const auto cl2 = uidPart(uid.data() + 3, false);
        listen(cl2);
        { std::vector<uint8_t> sel = {0x95, 0x70}; sel.insert(sel.end(), cl2.begin(), cl2.end()); poll(withCrc(sel)); }
        listen(withCrc({0x00}));
        poll(withCrc({0x30, 0x04}));
        listen(withCrc({0x03, 0x12, 0xD1, 0x01, 0x0E, 0x54, 0x02, 0x65, 0x6E, 0x68, 0x65, 0x6C, 0x6C, 0x6F, 0xFE, 0x00}));
        poll(withCrc({0x50, 0x00}));
        return f;
    }
    // MIFARE Classic 1K: 4-byte UID, ATQA 04 00, SAK 08
    poll({0x26}, 7);
    listen({0x04, 0x00});
    poll({0x93, 0x20});
    const auto cl1 = uidPart(uid.data(), false);
    listen(cl1);
    { std::vector<uint8_t> sel = {0x93, 0x70}; sel.insert(sel.end(), cl1.begin(), cl1.end()); poll(withCrc(sel)); }
    listen(withCrc({0x08}));
    poll(withCrc({0x50, 0x00}));
    return f;
}

struct NfcGenerator::Impl {
    NfcGenConfig cfg;
    std::vector<float> env;      // one period of the field's magnitude, relative to the carrier
    size_t pos = 0;
    double phase = 0, dphase = 0;
    genutil::NoiseSource noise;
    float sigma = 0;

    explicit Impl(const NfcGenConfig& c) : cfg(c), noise(c.seed) {
        sigma = (float)noiseSigma(c);
        dphase = 2 * math::kPi * c.cfoHz / c.rate;
        build();
    }

    void build() {
        const size_t periodN = (size_t)std::max(1.0, std::round(cfg.periodSec * cfg.rate));
        env.assign(periodN, 1.f);
        if (cfg.noiseOnly) { std::fill(env.begin(), env.end(), 0.f); return; }
        std::vector<Seg> segs;
        const double pauseFc = cfg.pauseUs * 1e-6 * kFc;
        double t = cfg.leadSec * kFc;
        const auto frames = nfcTestExchange(cfg.scene);
        for (size_t i = 0; i < frames.size(); i++) {
            const NfcGenFrame& fr = frames[i];
            if (i && fr.poll) t += frames[i - 1].poll ? kBetweenFc : kFdtReaderFc;
            if (fr.poll && fr.bytes.size() == 4 && fr.bytes[0] == 0x50) t += kBetweenFc;   // the HLTA comes a little later
            if (!fr.poll) t += kFdtCardFc;
            const std::vector<int> bits = frameBits(fr.bytes, fr.bits);
            t = fr.poll ? miller(segs, t, bits, pauseFc) : manchester(segs, t, bits);
        }
        const double toSample = cfg.rate / kFc;
        for (const Seg& s : segs) {
            const size_t a = (size_t)std::max(0.0, std::floor(s.start * toSample)), b = std::min(periodN, (size_t)std::ceil(s.end * toSample));
            for (size_t k = a; k < b; k++) {
                if (s.kind == 1) env[k] = 0.f;
                else {
                    const double tFc = (double)k / toSample;   // the subcarrier: fc / 16, so 8 carrier cycles per half cycle
                    const bool hi = ((long long)std::floor(tFc / 8.0) & 1) == 0;
                    env[k] = (float)(1.0 + (hi ? cfg.depth : -cfg.depth));
                }
            }
        }
    }
};

NfcGenerator::NfcGenerator(const NfcGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
NfcGenerator::~NfcGenerator() = default;

double NfcGenerator::noiseSigma(const NfcGenConfig& c) {
    return c.amp / std::sqrt(2.0 * std::pow(10.0, c.snrDb / 10.0));
}

void NfcGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const float amp = (float)m.cfg.amp;
    for (size_t i = 0; i < n; i++) {
        const float a = amp * m.env[m.pos];
        out[i] = cf32(a * (float)std::cos(m.phase), a * (float)std::sin(m.phase));
        m.phase += m.dphase;
        if (m.phase > math::kPi) m.phase -= 2 * math::kPi; else if (m.phase < -math::kPi) m.phase += 2 * math::kPi;
        if (++m.pos >= m.env.size()) m.pos = 0;
    }
    if (m.sigma > 0) m.noise.add(out, n, m.sigma);
}

namespace {
class NfcSynth : public ModeSynth {
public:
    NfcSynth(const SynthConfig& s, double rate) : rate_(rate) {
        NfcGenConfig c;
        c.rate = rate;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        const int o = s.modeOpt[0];
        c.noiseOnly = o == 2;
        c.scene = o == 1 ? 1 : 0;
        if (s.modeVal[0] > 0 && s.modeVal[0] < 1) c.depth = s.modeVal[0];
        gen_ = std::make_unique<NfcGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<NfcGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeNfcSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<NfcSynth>(cfg, sampleRate);
}

} // namespace dect2
