// EDACS receiver: FM front end at 76.8 kHz, the 48-bit sync (edacs::kSync or its complement) searched on hard decisions at every sample
// phase, then the 240 payload bits sliced at the bit centres the sync gives, voted (three copies) and checked (BCH(40,28)) per message,
// in the polarity the BCH accepts. Frame handling after DSD-FME's src/edacs-fme.c (ISC licence), in our own code.
// Working channels: the FM voice through AnalogVoice, the subaudible low-speed data's level, and ProVoice detection (zero crossings that keep
// to the 9600 bit/s clock while no EDACS frame decodes).
#include "dect2/edacs/edacs_rx.h"
#include "dect2/dsp/analog_voice.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/edacs/edacs_proto.h"
#include "dect2/edacs/edacs_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <ctime>
#include <map>
#include <mutex>

namespace dect2 {

ModeTuning edacsTuning() {
    ModeTuning t;
    t.stdMode = 132;
    t.id = "edacs";
    t.name = "EDACS trunking";
    t.minMhz = 29.7;
    t.maxMhz = 960.0;
    t.defMhz = 856.0;
    t.sampleRate = 480000;
    t.basebandHz = 300000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 307200;
    t.tuneOffsetHz = 100000;
    return t;
}

namespace {
constexpr double kWork = 76800.0;
constexpr int kRing = 8192, kMask = kRing - 1;
constexpr int kPayloadBits = 240;
constexpr int kSyncErrs = 3;              // sync bits that may be wrong
constexpr double kMinRangeHz = 400.0;
constexpr double kLockSec = 5.0;
std::atomic<uint64_t> gMsgSeq{0};
}

struct EdacsReceiver::Impl {
    std::mutex mu;
    EdacsTelemetry tel;
    std::function<void(const std::string&)> log;
    FmFront fm;
    FmBiquad post[2], lsdLp[2];
    EyeEnvelope env;
    AnalogVoice voice;
    std::vector<float> disc;
    double fs = 0, offHz = 0, centreHz = 0;
    bool rateSet = false, inverted = false, narrow = false;
    int sps = 8;
    uint64_t n = 0;
    float ring[kRing] = {};
    uint64_t reg[16] = {};                // last 48 hard bits per phase
    struct Hit { uint64_t n; bool inv; };
    std::vector<Hit> hits;
    uint64_t firstHit = 0, evalAt = 0, centre = 0, quietUntil = 0, dotQuiet = 0;
    bool pending = false, pendInv = false;
    double lastGoodSec = -1e9, lastPub = 0;
    std::map<uint32_t, uint64_t> wordCount;
    // working channel measures, per report period
    double lsdPow = 0; uint64_t lsdN = 0;
    std::complex<double> xsum = 0; uint64_t xN = 0; double prevSign = 0;
    double blockStart = 0;

    double timeSec() const { return (double)n / kWork; }

    void clearState() {
        fm.reset(); for (auto& b : post) b.clear(); for (auto& b : lsdLp) b.clear(); env.clear(); voice.reset();
        n = 0; for (auto& r : ring) r = 0; for (auto& r : reg) r = 0;
        hits.clear(); pending = false; firstHit = evalAt = centre = quietUntil = dotQuiet = 0; lastGoodSec = -1e9; lastPub = 0; wordCount.clear();
        lsdPow = 0; lsdN = 0; xsum = 0; xN = 0; prevSign = 0; blockStart = 0;
        const uint64_t s = tel.seq;
        tel = EdacsTelemetry();
        tel.seq = s + 1; tel.inputRate = fs; tel.centerHz = centreHz; tel.channelHz = centreHz + offHz; tel.state = rateSet ? 1 : 0;
        tel.inverted = inverted; tel.narrow = narrow;
    }

    void setRates() {
        sps = narrow ? 16 : 8;
        const double lp = narrow ? 3500.0 : 6000.0;
        post[0].lowpass(lp, kWork, 0.5412); post[1].lowpass(lp, kWork, 1.3066);
        lsdLp[0].lowpass(200.0, kWork, 0.5412); lsdLp[1].lowpass(200.0, kWork, 1.3066);
    }

    double soft(uint64_t idx) const { return (ring[idx & kMask] + ring[(idx - 1) & kMask] + ring[(idx - 2) & kMask]) / 3.0; }

    void evaluate() {
        // the last sync bit was sampled at centre - 1: payload bit i at centre - 1 + (i + 1) * sps
        const double thr = env.mid();
        uint8_t raw[kPayloadBits];
        for (int i = 0; i < kPayloadBits; i++) raw[i] = soft((uint64_t)((int64_t)centre - 1 + (int64_t)(i + 1) * sps)) > thr;
        tel.syncs++;
        if (env.range() < kMinRangeHz) return;
        // the polarity the sync suggested first; the BCH check settles it (dsd-fme accepts a frame only when both messages pass)
        edacs::Decoded a, b;
        bool inv = pendInv;
        for (int attempt = 0; attempt < 2; attempt++, inv = !inv) {
            uint8_t bits[kPayloadBits];
            for (int i = 0; i < kPayloadBits; i++) bits[i] = (uint8_t)(raw[i] != inv);
            a = edacs::decodeMessage(bits); b = edacs::decodeMessage(bits + 120);
            if (a.ok || b.ok) break;
        }
        for (const edacs::Decoded* d : {&a, &b}) {
            if (!d->ok) tel.msgBad++;
            else if (d->corrected) tel.bchFixed++;
            else tel.bchOk++;
        }
        if (!a.ok && !b.ok) { tel.blocksBad++; return; }
        inverted = inv;
        tel.inverted = inverted;
        EdacsFrame f;
        f.wallTime = (int64_t)std::time(nullptr);
        f.timeSec = timeSec();
        f.msgA = a.data; f.msgB = b.data; f.errA = a.disagree; f.errB = b.disagree;
        f.okA = a.ok; f.okB = b.ok; f.bchA = a.bchOk; f.bchB = b.bchOk; f.good = a.ok && b.ok;
        if (tel.frames.size() >= kEdacsLogMax) tel.frames.pop_back();
        tel.frames.insert(tel.frames.begin(), f);
        if (a.ok) wordCount[a.data]++;
        if (b.ok) wordCount[b.data]++;
        if (a.ok && b.ok) {
            EdacsMsgRec r;
            r.seq = ++gMsgSeq; r.m1 = a.data; r.m2 = b.data; r.timeSec = f.timeSec;
            tel.msgs.push_back(r);
            if (tel.msgs.size() > kEdacsMsgMax) tel.msgs.erase(tel.msgs.begin(), tel.msgs.end() - (long)kEdacsMsgMax);
        }
        std::vector<EdacsWordCount> w;
        for (auto& kv : wordCount) w.push_back({kv.first, kv.second});
        std::sort(w.begin(), w.end(), [](const EdacsWordCount& x, const EdacsWordCount& y) { return x.count > y.count; });
        if (w.size() > 24) w.resize(24);
        tel.words = std::move(w);
        tel.blocksOk++;
        tel.dataValid = true;
        lastGoodSec = f.timeSec;
        if (log) { char s[80]; snprintf(s, sizeof s, "EDACS frame %07X %07X", a.data, b.data); log(s); }
    }

    void sample(double s, double raw) {
        ring[n & kMask] = (float)s;
        env.push(s);
        const double thr = env.mid();
        const int ph = (int)(n % (uint64_t)sps);
        reg[ph] = ((reg[ph] << 1) | (soft(n) > thr ? 1ULL : 0ULL)) & 0xFFFFFFFFFFFFull;
        n++;
        // the working channel's measures
        const double l = lsdLp[1].run(lsdLp[0].run(raw));
        lsdPow += l * l; lsdN++;
        const double sg = s - thr;
        if (prevSign != 0 && (sg > 0) != (prevSign > 0)) {   // a crossing: its place in the 9600 bit/s clock
            const double phase = 2 * 3.14159265358979323846 * (double)(n % 8) / 8.0;
            xsum += std::polar(1.0, phase); xN++;
        }
        if (sg != 0) prevSign = sg;
        if (pending) {
            if (n >= evalAt) { pending = false; evaluate(); hits.clear(); }
            return;
        }
        if (n > 600 && env.range() >= kMinRangeHz) {   // end of transmission: 48 bits of dotting (dsd-fme DOTTING_SEQUENCE_A / B)
            const uint64_t r = reg[ph] & 0xFFFFFFFFFFFFull;
            if ((r == 0xAAAAAAAAAAAAull || r == 0x555555555555ull) && n >= dotQuiet) { tel.eot++; dotQuiet = n + 400 * (uint64_t)sps; }
        }
        if (n > 600 && n >= quietUntil && env.range() >= kMinRangeHz) {
            const uint64_t r = reg[ph];
            const int dn = __builtin_popcountll(r ^ edacs::kSync), di = __builtin_popcountll(r ^ (~edacs::kSync & 0xFFFFFFFFFFFFull));
            if (dn <= kSyncErrs || di <= kSyncErrs) {
                if (hits.empty()) firstHit = n;
                hits.push_back({n, di < dn});
            }
        }
        if (!hits.empty() && n >= firstHit + (uint64_t)sps / 2) {
            const Hit h = hits[hits.size() / 2];
            hits.clear();
            centre = h.n; pendInv = h.inv;
            pending = true;
            evalAt = centre + (uint64_t)(kPayloadBits + 3) * (uint64_t)sps;
        }
    }

    void publish() {
        tel.levelDb = (float)fm.levelDb();
        tel.deviationHz = (float)env.range();
        tel.timeSec = timeSec();
        tel.locked = timeSec() - lastGoodSec < kLockSec;
        tel.state = rateSet ? (tel.locked ? 2 : 1) : 0;
        tel.voice = voice.state();
        voice.markReported();
        tel.lsdHz = lsdN ? (float)std::sqrt(lsdPow / (double)lsdN) : 0.f;
        const double secs = std::max(1e-3, timeSec() - blockStart);
        const double rate = (double)xN / secs, coh = xN ? std::abs(xsum) / (double)xN : 0;
        tel.crossingCoherence = (float)coh;
        tel.lsd = tel.voice.carrier && tel.lsdHz > 120;
        // ProVoice: a carrier with 9600 bit/s data that is not the EDACS frame (no frame for a second)
        tel.provoice = tel.voice.carrier && !narrow && coh > 0.5 && rate > 2500 && timeSec() - lastGoodSec > 1.0;
        lsdPow = 0; lsdN = 0; xsum = 0; xN = 0; blockStart = timeSec();
        tel.seq++;
    }
};

EdacsReceiver::EdacsReceiver() : p_(std::make_unique<Impl>()) {}
EdacsReceiver::~EdacsReceiver() = default;

void EdacsReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = inputRateHz;
    p_->rateSet = inputRateHz >= edacsTuning().minSampleRate - 1 && p_->fm.configure(inputRateHz, kWork, 9000.0);
    p_->fm.setOffset(p_->offHz);
    p_->setRates();
    p_->voice.configure(kWork, 3000, 14000);   // the noise band above the 9600 bit/s data
    p_->clearState();
}
void EdacsReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offHz = hz; p_->fm.setOffset(hz); p_->tel.channelHz = p_->centreHz + hz; }
void EdacsReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->centreHz = hz; p_->tel.centerHz = hz; p_->tel.channelHz = hz + p_->offHz; }
void EdacsReceiver::setInverted(bool inv) { std::lock_guard<std::mutex> lk(p_->mu); p_->inverted = inv; p_->tel.inverted = inv; p_->tel.seq++; }
void EdacsReceiver::setNarrow(bool nb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->narrow == nb) return;
    p_->narrow = nb;
    p_->setRates();
    p_->clearState();
}
bool EdacsReceiver::ready() const { return p_->rateSet; }
void EdacsReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->clearState(); }
void EdacsReceiver::clearMessages() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.frames.clear(); p_->tel.words.clear(); p_->wordCount.clear(); p_->tel.dataValid = false; p_->tel.seq++;
}
void EdacsReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rateSet) { p_->tel.state = 0; return; }
    p_->disc.clear();
    p_->fm.feed(x, n, p_->disc);
    p_->voice.push(p_->disc.data(), p_->disc.size());
    for (float d : p_->disc) p_->sample(p_->post[1].run(p_->post[0].run(d)), d);
    if (p_->timeSec() - p_->lastPub >= 0.25) { p_->lastPub = p_->timeSec(); p_->publish(); }
}
bool EdacsReceiver::telemetry(EdacsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void EdacsReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void EdacsReceiver::setVoice(bool on) { p_->voice.setEnabled(on); }
void EdacsReceiver::setVolume(float v) { p_->voice.setVolume(v); }
void EdacsReceiver::setMuted(bool m) { p_->voice.setMuted(m); }
void EdacsReceiver::setSilent(bool s) { p_->voice.setSilent(s); }
void EdacsReceiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->voice.setTap(std::move(cb)); }

} // namespace dect2
