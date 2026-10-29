// LTR / Passport receiver: FM front end, 150 Hz low pass, decimate to 2400 Hz (8 samples per bit), integrate-and-dump slicing at every
// phase, and a search for the 9-bit sync 0x158 in both polarities, 40 bits back (an LTR word) and 68 bits back (a Passport word). A word
// is taken only when its checksum matches (SDRTrunk CRCLTR / CRCPassport, see ltr_proto.h). Neighbouring phases that hit the same word
// are merged (the middle one is used).
#include "dect2/ltr/ltr_rx.h"
#include "dect2/dsp/analog_voice.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/ltr/ltr_proto.h"
#include "dect2/ltr/ltr_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <map>
#include <string>
#include <mutex>

namespace dect2 {

ModeTuning ltrTuning() {
    ModeTuning t;
    t.stdMode = 131;
    t.id = "ltr";
    t.name = "LTR trunking";
    t.minMhz = 29.7;
    t.maxMhz = 960.0;
    t.defMhz = 460.0;
    t.sampleRate = 480000;
    t.basebandHz = 150000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 240000;
    t.tuneOffsetHz = 50000;
    return t;
}

namespace {
constexpr double kWork = 24000.0;      // the discriminator's rate: the voice (and the squelch's noise band) needs it
constexpr int kDecim = 10;
constexpr int kSps = 8;                 // samples per bit at 2400 Hz
constexpr double kMinRangeHz = 80.0;    // the data must swing at least this much (peak to peak) to be believed
constexpr double kLockSec = 15.0;
std::atomic<uint64_t> gWordSeq{0};
}

struct LtrReceiver::Impl {
    std::mutex mu;
    LtrTelemetry tel;
    std::function<void(const std::string&)> log;
    FmFront fm;
    FmBiquad lp[2];
    EyeEnvelope env;
    AnalogVoice voice;
    std::vector<float> disc;
    double fs = 0, offHz = 0, centreHz = 0;
    bool rateSet = false;
    int dcount = 0;
    uint64_t n = 0;                        // 2400 Hz sample counter
    double ring[512] = {};                  // last samples
    uint64_t lo[kSps] = {}, hi[kSps] = {};   // the last 128 bits per phase (hard decisions), newest in bit 0 of lo
    struct Hit { uint64_t n; uint64_t lo, hi; bool inv; };
    struct Det { int bits; std::vector<Hit> hits; uint64_t suppressUntil = 0, firstHit = 0; };
    Det det[2] = {{ltr::kWordBits, {}, 0, 0}, {ltr::kPassportBits, {}, 0, 0}};   // 0 LTR, 1 Passport
    double lastGoodSec = -1e9, lastPub = 0, lastLtrSec = -1e9, lastPassSec = -1e9;
    std::map<int, LtrRepeater> rep;
    uint64_t polCount[2] = {0, 0};   // normal-checksum words heard in each polarity (the outbound one is the larger)
    std::vector<std::unique_ptr<LtrReceiver>> watch;   // the other repeaters inside the capture
    std::vector<double> watchOff;
    std::vector<uint64_t> watchOk;                      // good words of each watched repeater already merged
    uint64_t mergedSeq = 0;                             // the last watched word merged into tel.words
    std::map<long long, double> lastWordAt;             // channel (Hz, rounded) -> time of its last good word

    double timeSec() const { return (double)n / (kWork / kDecim); }

    void clearState() {
        fm.reset(); lp[0].clear(); lp[1].clear(); env.clear(); voice.reset();
        n = 0; dcount = 0; for (auto& r : ring) r = 0; for (auto& r : lo) r = 0; for (auto& r : hi) r = 0;
        for (auto& d : det) { d.hits.clear(); d.suppressUntil = d.firstHit = 0; }
        lastGoodSec = lastLtrSec = lastPassSec = -1e9; lastPub = 0; rep.clear(); polCount[0] = polCount[1] = 0; lastWordAt.clear();
        const uint64_t s = tel.seq;
        tel = LtrTelemetry();
        tel.seq = s + 1; tel.inputRate = fs; tel.centerHz = centreHz; tel.channelHz = centreHz + offHz; tel.state = rateSet ? 1 : 0;
    }

    // every bit of the word, integrated over its 8 samples, must sit clearly on one side of the threshold
    bool eyeOk(const Hit& h, int nb) const {
        const double thr = env.mid(), half = 0.5 * env.range();
        for (int i = 0; i < nb; i++) {
            const uint64_t last = h.n - 1 - (uint64_t)(nb - 1 - i) * kSps;
            double sum = 0;
            for (int k = 0; k < kSps; k++) sum += ring[(last - (uint64_t)k) & 511];
            if (std::fabs(sum / kSps - thr) < 0.15 * half) return false;
        }
        return true;
    }

    void addMessage(const LtrMessage& m) {
        if (tel.messages.size() >= kLtrLogMax) tel.messages.pop_back();
        tel.messages.insert(tel.messages.begin(), m);
        tel.blocksOk++;
        tel.dataValid = true;
        lastGoodSec = m.timeSec;
    }

    void addWord(LtrWordRec& wr) {
        wr.seq = ++gWordSeq;
        lastWordAt[std::llround(wr.hz)] = wr.timeSec;
        tel.words.push_back(wr);
        if (tel.words.size() > kLtrWordMax) tel.words.erase(tel.words.begin(), tel.words.end() - (long)kLtrWordMax);
    }

    bool handleLtr(const uint8_t* bits, const Hit& h) {
        ltr::Word word;
        if (!ltr::parseWord(bits, word)) return false;
        tel.syncs++;
        if (env.range() < kMinRangeHz || !eyeOk(h, ltr::kWordBits)) return false;
        int c = ltr::checkWord(word);
        // The repeater's outbound words (OSW) carry the normal checksum; SDRTrunk (CRCLTR.check) accepts the inverted one only on an
        // inbound word (ISW: the whole word inverted, sent by a mobile). The outbound polarity is learned from the normal-checksum words;
        // an inverted checksum counts only on a word of the other polarity.
        if (c == 1) polCount[h.inv ? 1 : 0]++;
        const int osw = polCount[0] == polCount[1] ? -1 : polCount[1] > polCount[0] ? 1 : 0;
        if (c == 2 && (osw < 0 || (int)h.inv == osw)) c = 0;
        if (c == 0) { if (timeSec() - lastPassSec > 10) tel.blocksBad++; return false; }   // the first 40 bits of a Passport word are no LTR word
        lastLtrSec = timeSec();
        LtrMessage m;
        m.wallTime = (int64_t)std::time(nullptr);
        m.timeSec = timeSec();
        m.area = word.area; m.gotoCh = word.gotoCh; m.home = word.home; m.id = word.id; m.freeCh = word.freeCh; m.chk = word.chk;
        m.inbound = c == 2;
        m.hz = centreHz + offHz;
        m.talkgroup = ltr::talkgroup(word);
        // LTRStandardMessageProcessor: go-to 31 ends a call; go-to == free with group 255 is the idle word
        m.kind = word.gotoCh == ltr::kTurnOff ? "call end" : (word.gotoCh == word.freeCh && word.id == 255) ? "idle" : ltr::kindOf(word.id);
        auto touch = [&](int ch, int which) {
            if (ch < 1 || ch > 20) return;
            LtrRepeater& r = rep[ch];
            r.ch = ch; r.lastSec = m.timeSec;
            (which == 0 ? r.asGoto : which == 1 ? r.asHome : r.asFree)++;
        };
        touch(word.gotoCh, 0); touch(word.home, 1); touch(word.freeCh, 2);
        tel.repeaters.clear();
        for (auto& kv : rep) tel.repeaters.push_back(kv.second);
        tel.area = word.area;
        tel.passport = false;
        addMessage(m);
        LtrWordRec wr;
        wr.area = word.area; wr.gotoCh = word.gotoCh; wr.home = word.home; wr.id = word.id; wr.freeCh = word.freeCh; wr.timeSec = m.timeSec;
        wr.hz = m.hz;
        addWord(wr);
        if (word.gotoCh == ltr::kTurnOff) { tel.turnOffs++; tel.turnOffArea = word.area; tel.turnOffHome = word.home; tel.turnOffId = word.id; }
        if (log) log("LTR " + m.talkgroup + (m.kind.empty() ? "" : " " + m.kind) + (m.inbound ? " (inbound)" : ""));
        return true;
    }

    bool handlePassport(const uint8_t* bits, const Hit& h) {
        ltr::PassportWord p;
        if (!ltr::parsePassport(bits, p)) return false;
        if (env.range() < kMinRangeHz || !eyeOk(h, ltr::kPassportBits)) return false;
        if (!ltr::passportOk(p)) { if (timeSec() - lastLtrSec > 10) tel.blocksBad++; return false; }
        lastPassSec = timeSec();
        const ltr::PassportKind k = ltr::passportKind(p);
        LtrMessage m;
        m.wallTime = (int64_t)std::time(nullptr);
        m.timeSec = timeSec();
        m.passport = true;
        m.hz = centreHz + offHz;
        m.dcc = p.dcc; m.lcn = p.lcn; m.site = p.site; m.group = p.group; m.type = p.type; m.freeCh = p.freeCh; m.chk = p.chk;
        m.kind = ltr::passportKindName(k);
        if (k == ltr::PassportKind::RadioId) m.talkgroup = "radio " + std::to_string(p.radioId());
        else if (k != ltr::PassportKind::Idle) m.talkgroup = std::to_string(p.group);
        if (k == ltr::PassportKind::Idle) { tel.passportSite = p.site; tel.passportDcc = p.dcc; tel.passportBand = p.siteBand(); }
        tel.passport = true;
        addMessage(m);
        LtrWordRec wr;
        wr.passport = true; wr.hz = m.hz; wr.dcc = p.dcc; wr.lcn = p.lcn; wr.site = p.site; wr.group = p.group; wr.type = p.type; wr.freeCh = p.freeCh; wr.timeSec = m.timeSec;
        addWord(wr);
        if (k == ltr::PassportKind::CallEnd) { tel.turnOffs++; tel.turnOffGroup = p.group; }
        if (log) log("Passport " + m.kind + (m.talkgroup.empty() ? "" : " " + m.talkgroup) + (k == ltr::PassportKind::CallStart ? " LCN " + std::to_string(p.lcn) : ""));
        return true;
    }

    bool handle(int kind, const Hit& h) {
        const int nb = det[kind].bits;
        uint8_t bits[ltr::kPassportBits];
        for (int i = 0; i < nb; i++) {
            const int back = nb - 1 - i;   // how many bits before the newest
            const int b = back < 64 ? (int)((h.lo >> back) & 1) : (int)((h.hi >> (back - 64)) & 1);
            bits[i] = (uint8_t)(b ^ (h.inv ? 1 : 0));
        }
        return kind == 0 ? handleLtr(bits, h) : handlePassport(bits, h);
    }

    void sample(double s) {
        ring[n & 511] = s;
        env.push(s);
        const double thr = env.mid();
        double sum = 0;
        for (int k = 0; k < kSps; k++) sum += ring[(n - (uint64_t)k) & 511];
        const int ph = (int)(n % kSps);
        hi[ph] = (hi[ph] << 1) | (lo[ph] >> 63);
        lo[ph] = (lo[ph] << 1) | (sum / kSps > thr ? 1ULL : 0ULL);
        n++;
        for (int kind = 0; kind < 2; kind++) {
            Det& d = det[kind];
            if (n > (uint64_t)(d.bits + 10) * kSps && n >= d.suppressUntil) {
                const int back = d.bits - 9;   // the sync's lowest bit
                const uint32_t top = back + 9 <= 64 ? (uint32_t)((lo[ph] >> back) & 0x1FF)
                                                    : (uint32_t)((((hi[ph] << (64 - back)) | (lo[ph] >> back)) & 0x1FF));
                if (top == ltr::kSync || top == (~ltr::kSync & 0x1FF)) {
                    if (d.hits.empty()) d.firstHit = n;
                    d.hits.push_back({n, lo[ph], hi[ph], top != ltr::kSync});
                }
            }
            if (!d.hits.empty() && n >= d.firstHit + kSps / 2) {   // the hits of the neighbouring phases are in
                const Hit h = d.hits[d.hits.size() / 2];
                d.hits.clear();
                // after a good word the next one is at least a word away; after a bad one look again at once (it may have been a
                // sync pattern inside the data, with the real word about to follow)
                d.suppressUntil = handle(kind, h) ? n + (uint64_t)(d.bits - 2) * kSps : n + kSps;
            }
        }
    }

    void publish() {
        tel.levelDb = (float)fm.levelDb();
        tel.deviationHz = (float)env.range();
        tel.timeSec = timeSec();
        tel.locked = timeSec() - lastGoodSec < kLockSec;
        tel.state = rateSet ? (tel.locked ? 2 : 1) : 0;
        tel.snrDb = 0;
        tel.voice = voice.state();
        voice.markReported();
        tel.watching = 1 + (int)watch.size();
        tel.busyHz.clear();
        for (const auto& kv : lastWordAt) if (timeSec() - kv.second < 1.5) tel.busyHz.push_back((double)kv.first);
        tel.seq++;
    }
};

LtrReceiver::LtrReceiver() : p_(std::make_unique<Impl>()) {}
LtrReceiver::~LtrReceiver() = default;

void LtrReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = inputRateHz;
    p_->rateSet = inputRateHz >= ltrTuning().minSampleRate - 1 && p_->fm.configure(inputRateHz, kWork, 6000.0);
    p_->voice.configure(kWork, 3000, 5000);
    p_->fm.setOffset(p_->offHz);
    p_->lp[0].lowpass(150.0, kWork, 0.5412);
    p_->lp[1].lowpass(150.0, kWork, 1.3066);
    p_->clearState();
    for (auto& w : p_->watch) w->configure(inputRateHz);
    p_->watchOk.assign(p_->watch.size(), 0);
}
void LtrReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offHz = hz; p_->fm.setOffset(hz); p_->tel.channelHz = p_->centreHz + hz; }
void LtrReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->centreHz = hz; p_->tel.centerHz = hz; p_->tel.channelHz = hz + p_->offHz;
    for (auto& w : p_->watch) w->setCenterHz(hz);
}
bool LtrReceiver::ready() const { return p_->rateSet; }
void LtrReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->clearState();
    for (auto& w : p_->watch) w->reset();
    p_->watchOk.assign(p_->watch.size(), 0);
}
void LtrReceiver::clearMessages() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.messages.clear(); p_->tel.repeaters.clear(); p_->rep.clear(); p_->tel.dataValid = false; p_->tel.seq++;
}
void LtrReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rateSet) { p_->tel.state = 0; return; }
    p_->disc.clear();
    p_->fm.feed(x, n, p_->disc);
    p_->voice.push(p_->disc.data(), p_->disc.size());
    for (float d : p_->disc) {
        const double y = p_->lp[1].run(p_->lp[0].run(d));
        if (++p_->dcount == kDecim) { p_->dcount = 0; p_->sample(y); }
    }
    // the watched repeaters: their words join this receiver's, in sequence order (the numbers are shared, so the order is the order heard)
    if (!p_->watch.empty()) {
        bool added = false;
        for (auto& w : p_->watch) {
            w->feed(x, n);
            LtrTelemetry wt;
            if (!w->telemetry(wt, 0)) continue;
            for (const LtrWordRec& r : wt.words) if (r.seq > p_->mergedSeq) {
                p_->tel.words.push_back(r);
                p_->lastWordAt[std::llround(r.hz)] = p_->timeSec();
                added = true;
            }
            const size_t k = (size_t)(&w - p_->watch.data());
            if (wt.blocksOk > p_->watchOk[k]) {   // its newest messages (newest first) since the last merge
                const size_t fresh = std::min<size_t>((size_t)(wt.blocksOk - p_->watchOk[k]), wt.messages.size());
                for (size_t i = fresh; i-- > 0;) p_->tel.messages.insert(p_->tel.messages.begin(), wt.messages[i]);
                p_->tel.blocksOk += wt.blocksOk - p_->watchOk[k];
                p_->watchOk[k] = wt.blocksOk;
                p_->tel.dataValid = true;
                p_->lastGoodSec = p_->timeSec();
            }
        }
        if (added) {
            std::sort(p_->tel.words.begin(), p_->tel.words.end(), [](const LtrWordRec& a, const LtrWordRec& b) { return a.seq < b.seq; });
            if (p_->tel.words.size() > kLtrWordMax) p_->tel.words.erase(p_->tel.words.begin(), p_->tel.words.end() - (long)kLtrWordMax);
            p_->mergedSeq = p_->tel.words.back().seq;
        }
        if (p_->tel.messages.size() > kLtrLogMax) p_->tel.messages.resize(kLtrLogMax);
    }
    if (p_->timeSec() - p_->lastPub >= 0.25) { p_->lastPub = p_->timeSec(); p_->publish(); }
}

void LtrReceiver::setWatch(const std::vector<double>& offsetsHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (offsetsHz == p_->watchOff) return;
    p_->watchOff = offsetsHz;
    p_->watch.clear();
    p_->watchOk.assign(offsetsHz.size(), 0);
    for (double o : offsetsHz) {
        auto r = std::make_unique<LtrReceiver>();
        r->configure(p_->fs);
        r->setCenterHz(p_->centreHz);
        r->setSignalOffset(o);
        r->setVoice(false);
        r->setSilent(true);
        p_->watch.push_back(std::move(r));
    }
}
bool LtrReceiver::telemetry(LtrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void LtrReceiver::setVoice(bool on) { p_->voice.setEnabled(on); }
void LtrReceiver::setVolume(float v) { p_->voice.setVolume(v); }
void LtrReceiver::setMuted(bool m) { p_->voice.setMuted(m); }
void LtrReceiver::setSilent(bool s) { p_->voice.setSilent(s); }
void LtrReceiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->voice.setTap(std::move(cb)); }
void LtrReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

} // namespace dect2
