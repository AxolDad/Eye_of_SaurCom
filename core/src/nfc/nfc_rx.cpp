// NFC / RFID sniffer: see nfc_rx.h. The demodulation is nfc-laboratory's (third_party/nfc-laboratory, GPL-3.0); this file feeds it the
// magnitude of the samples and turns its RawFrames into the telemetry.
#include "dect2/nfc/nfc_rx.h"
#include "dect2/nfc/nfc_proto.h"
#include "dect2/nfc/nfc_tel.h"
#include <hw/SignalBuffer.h>
#include <hw/SignalType.h>
#include <lab/data/RawFrame.h>
#include <lab/nfc/NfcDecoder.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <mutex>

namespace dect2 {

ModeTuning nfcTuning() {
    ModeTuning t;
    t.stdMode = 150;
    t.id = "nfc";
    t.name = "NFC / RFID sniffer";
    t.minMhz = 13.0;
    t.maxMhz = 14.2;
    t.defMhz = 13.56;
    t.sampleRate = 10e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 2.0;
    t.minSampleRate = 2.5e6;
    t.tuneOffsetHz = 0;          // the field's carrier is the signal: it sits on the radio's centre
    t.carrierAtCentre = true;    // and the DC removal must leave it alone
    t.notCircular = true;        // an amplitude-modulated carrier is no basis for a blind IQ-imbalance estimate
    return t;
}

struct NfcReceiver::Impl {
    std::mutex mu;
    NfcTelemetry tel;
    std::function<void(const std::string&)> log;
    std::unique_ptr<lab::NfcDecoder> dec;
    double rate = 0, centreHz = 0;
    bool rateSet = false;
    uint64_t total = 0;          // samples fed: the decoder's clock is this, 32 bits wide
    std::vector<float> mag;
    nfc::Anticollision ac;
    int lastPollA = -1, lastPollB = -1, lastPollF = -1, lastPollV = -1;
    double levelAcc = 0; size_t levelN = 0;
    double lastPub = -1, lastFrameSec = -1e9;
    int techMask = 15;
    float powerThr = 0.01f;
    bool changed = false;

    double timeSec() const { return rate > 0 ? (double)total / rate : 0; }

    void makeDecoder() {
        dec = std::make_unique<lab::NfcDecoder>();
        dec->setSampleRate((long)std::lround(rate));
        dec->setPowerLevelThreshold(powerThr);
        applyTech();
        dec->initialize();
        ac.reset();
        lastPollA = lastPollB = lastPollF = lastPollV = -1;
    }
    void applyTech() {
        if (!dec) return;
        dec->setEnableNfcA(techMask & 1); dec->setEnableNfcB(techMask & 2); dec->setEnableNfcF(techMask & 4); dec->setEnableNfcV(techMask & 8);
    }

    // The decoder's sample clock is 32 bits wide; a frame's start is turned back into a 64-bit index from the count after this buffer
    uint64_t absSample(unsigned long clk, uint64_t after) const { return after - (uint32_t)((uint32_t)after - (uint32_t)clk); }

    NfcCard* findCard(nfc::Tech tech, const std::vector<uint8_t>& uid, size_t* idx) {
        for (size_t i = 0; i < tel.cards.size(); i++) if (tel.cards[i].tech == tech && tel.cards[i].uid == uid) { *idx = i; return &tel.cards[i]; }
        return nullptr;
    }

    void card(nfc::Tech tech, const std::vector<uint8_t>& uid, const uint8_t* atqa, uint8_t sak, const std::string& type, double sec) {
        if (uid.empty()) return;
        const int64_t now = (int64_t)std::time(nullptr);
        size_t idx = 0;
        NfcCard* c = findCard(tech, uid, &idx);
        const bool fresh = !c;
        if (!c) {
            NfcCard n;
            n.tech = tech; n.uid = uid; n.firstWall = now;
            n.manufacturer = (tech == nfc::TechA || tech == nfc::TechV) ? nfc::uidManufacturer(tech == nfc::TechV && uid.size() == 8 ? uid[1] : uid[0]) : "";
            tel.cards.insert(tel.cards.begin(), n);
            if (tel.cards.size() > kNfcCardMax) tel.cards.pop_back();
            c = &tel.cards[0]; idx = 0;
        }
        if (atqa) { c->atqa[0] = atqa[0]; c->atqa[1] = atqa[1]; }
        c->sak = sak;
        if (!type.empty()) c->type = type;
        c->iso14443_4 = tech == nfc::TechA ? (sak & 0x20) != 0 : tech == nfc::TechB;
        c->lastWall = now; c->lastSec = sec; c->selections++;
        if (idx) std::rotate(tel.cards.begin(), tel.cards.begin() + (ptrdiff_t)idx, tel.cards.begin() + (ptrdiff_t)idx + 1);
        if (fresh && log) log("NFC: " + std::string(nfc::techName(tech)) + " card, UID " + nfc::hex(uid.data(), uid.size()) + (type.empty() ? "" : ", " + type));
        changed = true;
    }

    void handle(const lab::RawFrame& f, uint64_t after) {
        const unsigned ft = f.frameType();
        const double sec = rate > 0 ? (double)absSample(f.sampleStart(), after) / rate : 0;
        if (ft == lab::NfcCarrierOn || ft == lab::NfcCarrierOff) {
            const bool on = ft == lab::NfcCarrierOn;
            if (on && !tel.fieldOn) { tel.fieldOnCount++; tel.fieldSec = sec; if (log && tel.fieldOnCount <= 3) log("NFC: reader field detected"); }
            tel.fieldOn = on;
            changed = true;
            return;
        }
        if (ft != lab::NfcPollFrame && ft != lab::NfcListenFrame) return;
        NfcFrame r;
        r.wallTime = (int64_t)std::time(nullptr);
        r.sec = sec;
        r.durationUs = rate > 0 ? (double)(f.sampleEnd() - f.sampleStart()) / rate * 1e6 : 0;
        r.poll = ft == lab::NfcPollFrame;
        switch (f.techType()) {
        case lab::NfcATech: r.tech = nfc::TechA; break;
        case lab::NfcBTech: r.tech = nfc::TechB; break;
        case lab::NfcFTech: r.tech = nfc::TechF; break;
        case lab::NfcVTech: r.tech = nfc::TechV; break;
        default: r.tech = nfc::TechUnknown; break;
        }
        r.rateKbps = f.frameRate() > 0 ? (int)std::lround(f.frameRate() / 1000.0) : 106;
        const unsigned fl = f.frameFlags();
        r.parityError = fl & lab::ParityError; r.crcError = fl & lab::CrcError; r.truncated = fl & lab::Truncated;
        r.encrypted = fl & lab::Encrypted; r.shortFrame = fl & lab::ShortFrame;
        const unsigned char* p = f.data();
        const size_t n = f.limit();
        r.bytes.assign(p, p + n);
        if (r.shortFrame && n == 1) r.bits = r.poll ? 7 : 4;
        switch (r.tech) {
        case nfc::TechA: {
            r.name = nfc::nameA(p, n, r.poll, lastPollA, r.bits);
            if (r.encrypted) r.name = "encrypted (" + std::to_string(n) + " bytes)";
            const bool done = ac.frame(p, n, r.poll);
            if (r.poll) lastPollA = n ? p[0] : -1;
            if (done) {
                const uint8_t atqa[2] = {ac.atqa0(), ac.atqa1()};
                const nfc::CardInfo ci = nfc::cardInfo(atqa[0], atqa[1], ac.sak());
                card(nfc::TechA, ac.uid(), atqa, ac.sak(), ci.type, sec);
            }
            if (r.encrypted && !tel.cards.empty()) tel.cards[0].encryptedSeen = true;
            break;
        }
        case nfc::TechB:
            r.name = nfc::nameB(p, n, r.poll);
            if (r.poll) lastPollB = n ? p[0] : -1;
            else if (lastPollB == 0x05 && n >= 14 && p[0] == 0x50) card(nfc::TechB, std::vector<uint8_t>(p + 1, p + 5), nullptr, 0, "ISO 14443-B card (PUPI)", sec);
            break;
        case nfc::TechF:
            r.name = nfc::nameF(p, n, r.poll);
            if (r.poll) lastPollF = n > 1 ? p[1] : -1;
            else if (n >= 18 && p[1] == 0x01) card(nfc::TechF, std::vector<uint8_t>(p + 2, p + 10), nullptr, 0, "FeliCa / NFC-F (IDm)", sec);
            break;
        case nfc::TechV:
            r.name = nfc::nameV(p, n, r.poll);
            if (r.poll) lastPollV = n > 1 ? p[1] : -1;
            else if (lastPollV == 0x01 && n >= 12 && !(p[0] & 1)) {
                std::vector<uint8_t> u(8);
                for (int i = 0; i < 8; i++) u[(size_t)i] = p[9 - i];
                card(nfc::TechV, u, nullptr, 0, "ISO 15693 / NFC-V tag", sec);
            }
            break;
        default: r.name = std::to_string(n) + " bytes"; break;
        }
        if (r.poll) tel.pollFrames++; else tel.listenFrames++;
        switch (r.tech) { case nfc::TechA: tel.framesA++; break; case nfc::TechB: tel.framesB++; break; case nfc::TechF: tel.framesF++; break; case nfc::TechV: tel.framesV++; break; default: break; }
        if (r.parityError || r.crcError) tel.blocksBad++; else tel.blocksOk++;
        lastFrameSec = sec;
        tel.frames.insert(tel.frames.begin(), std::move(r));
        if (tel.frames.size() > kNfcLogMax) tel.frames.pop_back();
        changed = true;
    }

    void publish() {
        tel.timeSec = timeSec();
        tel.inputRate = rate;
        tel.centerHz = centreHz;
        tel.techMask = techMask;
        tel.powerThreshold = powerThr;
        if (levelN) { tel.levelDb = (float)(10 * std::log10(std::max(1e-20, levelAcc / (double)levelN))); levelAcc = 0; levelN = 0; }
        tel.dataValid = tel.timeSec - lastFrameSec < 5.0;
        tel.state = rateSet ? (tel.dataValid ? 2 : 1) : 0;
        tel.seq++;
        changed = false;
        lastPub = tel.timeSec;
    }
};

NfcReceiver::NfcReceiver() : p_(std::make_unique<Impl>()) {}
NfcReceiver::~NfcReceiver() = default;

void NfcReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->rateSet = inputRateHz >= nfcTuning().minSampleRate - 1;
    p_->total = 0;
    p_->lastPub = -1; p_->lastFrameSec = -1e9;
    p_->tel.fieldOn = false;
    if (p_->rateSet) p_->makeDecoder(); else p_->dec.reset();
    p_->tel.state = p_->rateSet ? 1 : 0;
    p_->tel.seq++;
}
void NfcReceiver::setSignalOffset(double) {}
void NfcReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->centreHz = hz; p_->tel.centerHz = hz; }
bool NfcReceiver::ready() const { return p_->rateSet; }
void NfcReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->rateSet) p_->makeDecoder();
    p_->tel.fieldOn = false;
    p_->changed = true;
}
void NfcReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.frames.clear(); p_->tel.cards.clear();
    p_->tel.blocksOk = p_->tel.blocksBad = 0;
    p_->tel.pollFrames = p_->tel.listenFrames = 0;
    p_->tel.framesA = p_->tel.framesB = p_->tel.framesF = p_->tel.framesV = 0;
    p_->ac.reset();
    p_->tel.seq++;
}
void NfcReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void NfcReceiver::setTechMask(int mask) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->techMask = mask & 15 ? mask & 15 : 15;
    p_->applyTech();
    p_->tel.techMask = p_->techMask;
    p_->tel.seq++;
}
void NfcReceiver::setPowerThreshold(float mag) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->powerThr = std::max(0.0005f, std::min(0.5f, mag));
    if (p_->dec) p_->makeDecoder();   // the carrier thresholds are set when the decoder is made; it starts over, the frames and cards stay
    p_->tel.fieldOn = false;
    p_->tel.powerThreshold = p_->powerThr;
    p_->tel.seq++;
}

void NfcReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.mu);
    if (!m.rateSet || !m.dec) { m.tel.state = 0; return; }
    m.mag.resize(n);
    for (size_t i = 0; i < n; i++) {
        const float re = x[i].real(), im = x[i].imag();
        const float p = std::isfinite(re) && std::isfinite(im) ? re * re + im * im : 0.f;
        m.mag[i] = std::sqrt(p);
        m.levelAcc += p;
    }
    m.levelN += n;
    hw::SignalBuffer buf(m.mag.data(), (unsigned)n, 1, 1, (unsigned)std::lround(m.rate), m.total, 0, hw::SignalType::SIGNAL_TYPE_RADIO_SAMPLES);
    const std::list<lab::RawFrame> frames = m.dec->nextFrames(buf);
    m.total += n;
    for (const lab::RawFrame& f : frames) m.handle(f, m.total);
    if (m.lastPub < 0 || m.timeSec() - m.lastPub >= 0.1 || (m.changed && m.timeSec() - m.lastPub >= 0.02)) m.publish();
}

bool NfcReceiver::telemetry(NfcTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

} // namespace dect2
