// KAAL transmit generator: see kaal_txgen.h.
#include "dect2/kaal/kaal_txgen.h"
#include "dect2/kaal/kaal_tx.h"
#include "dect2/kaal/kaal_codec3.h"
#include "dect2/kaal/kaal_sweepid.h"
#include "dect2/kaal/kaal_beacon.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {
using dect2::math::kPi;

bool kaalBeaconFromRequest(const TxRequest& r, std::string& callsign, std::string& grid, double& powerDbm, std::string& text, std::string& err) {
    callsign.clear();
    for (char c : r.callsign) callsign += (char)std::toupper((unsigned char)c);
    while (!callsign.empty() && callsign.back() == ' ') callsign.pop_back();
    if (callsign.empty()) callsign = "ZORCOM";   // an empty field is the demo identity, like every other mode
    if (callsign.size() > 9) { err = "KAAL: the callsign is at most 9 characters"; return false; }
    for (char c : callsign) {
        if (!(c == ' ' || c == '/' || c == '-' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) { err = "KAAL: the callsign may hold only letters, digits, space, / and -"; return false; }
    }
    grid = r.grid.empty() ? std::string("AA00aa") : r.grid;
    if (grid.size() != 6) { err = "KAAL: the grid has 6 characters, for example KO26ab"; return false; }
    for (int i = 0; i < 6; i++) {
        const char c = grid[(size_t)i];
        const bool ok = i < 2 ? std::isalpha((unsigned char)c) : i < 4 ? std::isdigit((unsigned char)c) : std::isalpha((unsigned char)c);
        if (!ok) { err = "KAAL: the grid has 6 characters, for example KO26ab"; return false; }
        grid[(size_t)i] = (char)(i < 2 ? std::toupper((unsigned char)c) : i < 4 ? c : std::tolower((unsigned char)c));
    }
    powerDbm = std::max(-40.0, std::min(87.5, std::round(r.o("kaal.powerDbm", 10.0) * 2.0) / 2.0));
    text = r.text.substr(0, kaal::kBeaconMaxText);
    return true;
}

namespace {

// 48 kHz stereo from the session to 16 kHz mono: a 21-tap Hamming-windowed sinc low-pass at 7 kHz, then every third sample
class Mono16 {
public:
    Mono16() {
        const int n = 21, mid = n / 2;
        h_.assign((size_t)n, 0.f);
        double sum = 0;
        for (int i = 0; i < n; i++) {
            const double x = (double)(i - mid);
            const double fc = 7000.0 / 48000.0;
            const double s = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
            const double w = 0.54 - 0.46 * std::cos(2 * kPi * (double)i / (double)(n - 1));
            h_[(size_t)i] = (float)(s * w);
            sum += s * w;
        }
        for (auto& v : h_) v = (float)(v / sum);
        hist_.assign(h_.size(), 0.f);
    }
    // `frames` stereo frames in; appends frames / 3 samples
    void process(const float* stereo, int frames, std::vector<int16_t>& out) {
        for (int i = 0; i < frames; i++) {
            hist_[pos_] = 0.5f * (stereo[2 * i] + stereo[2 * i + 1]);
            pos_ = (pos_ + 1) % hist_.size();
            if (++phase_ == 3) {
                phase_ = 0;
                float y = 0;
                for (size_t k = 0; k < h_.size(); k++) y += h_[k] * hist_[(pos_ + hist_.size() - 1 - k) % hist_.size()];
                y = std::max(-1.f, std::min(1.f, y));
                out.push_back((int16_t)std::lrintf(y * 32767.f));
            }
        }
    }
private:
    std::vector<float> h_, hist_;
    size_t pos_ = 0;
    int phase_ = 0;
};

class KaalGen : public TxGenerator {
public:
    KaalGen(double rate, std::function<int(float*, int)> pull) : rate_(rate), pull_(std::move(pull)) { tx_.configure(rate); }
    kaal::VoiceTx tx_;
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        size_t have = 0;
        int guard = 0;
        while (have < n) {
            have += tx_.popIq(out + have, n - have);
            if (have >= n) break;
            if (++guard > 4000) { std::fill(out + have, out + n, cf32(0.f, 0.f)); break; }   // never wait forever
            feedMore();
        }
    }
private:
    void feedMore() {
        constexpr int kFrames = 960;   // 20 ms
        stereo_.assign((size_t)kFrames * 2, 0.f);
        if (pull_) pull_(stereo_.data(), kFrames);
        pcm_.clear();
        mono_.process(stereo_.data(), kFrames, pcm_);
        tx_.pushAudio(pcm_.data(), pcm_.size());
    }
    double rate_;
    std::function<int(float*, int)> pull_;
    Mono16 mono_;
    std::vector<float> stereo_;
    std::vector<int16_t> pcm_;
};

} // namespace

std::unique_ptr<TxGenerator> makeKaalTxGenerator(const TxRequest& r, double rateHz, std::string& err) {
    if (rateHz < 48000.0 - 1) { err = "KAAL needs a transmit rate of at least 48 kHz"; return nullptr; }
    auto g = std::make_unique<KaalGen>(rateHz, r.pullAudio);
    const int kb = (int)std::lround(r.o("kaal.bitrate", 16.0));
    if (!KaalCodec3::validBitrate(kb)) { err = "KAAL: the voice bitrate is 12, 16, 24 or 32 kbit/s"; return nullptr; }
    g->tx_.setBitrate(kb);
    {   // the callsign also goes into the sweep (6 characters + a suffix class); one that does not fit leaves the sweep without an ID
        std::string cs;
        for (char c : r.callsign) cs += (char)std::toupper((unsigned char)c);
        while (!cs.empty() && cs.back() == ' ') cs.pop_back();
        if (cs.empty()) cs = "ZORCOM";
        g->tx_.setSweepCallsign(cs);
    }
    const bool beacon = r.o("kaal.beacon", 1) > 0.5;
    if (beacon) {
        std::string cs, gr, text;
        double pw = 0;
        if (!kaalBeaconFromRequest(r, cs, gr, pw, text, err)) return nullptr;
        kaal::BeaconFields f;
        f.callsign = cs; f.grid = gr; f.lat = r.lat; f.lon = r.lon; f.powerDbm = pw;
        f.linkQuality = (int)r.o("kaal.linkQuality", 0);
        f.frameType = text.empty() ? 0 : 1;
        if (kaal::encodeBeaconBits(f, text).empty()) { err = "KAAL: the beacon fields cannot be encoded (check the callsign, the grid and the position)"; return nullptr; }
        g->tx_.setBeacon(f, text);
    }
    g->tx_.setBeaconMix(beacon);
    return g;
}

} // namespace dect2
