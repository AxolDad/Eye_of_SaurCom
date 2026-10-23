// From HDC audio packets to sound (see hdr_hdc.h). The packet buffering follows the way nrsc5 (GPL-3.0, github.com/theori-io/nrsc5, src/output.c
// and src/frame.c) plays a stream: every program has a ring of 64 packet slots indexed by the packet sequence number, each PDU header says
// where the play position stands (align), and the play position moves on by one slot per 46.4 ms of signal. A slot that is empty or has a bad
// CRC is silence and restarts the decoder, since HDC frames depend on the one before. The code here is Eye of ZorCom's own.
#include "dect2/hdr/hdr_hdc.h"
#include "dect2/audio/audioout.h"
#include "dect2/dsp/resampler.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <mutex>

namespace dect2 { namespace hdr {

namespace {
constexpr int kSlots = 64;                          // nrsc5 ELASTIC_BUFFER_LEN
constexpr int kPrograms = 8;
constexpr double kStep = kHdcFrameSamples / kHdcRate;   // seconds of signal per packet
constexpr int kRepeatedErrors = 3;                  // decode errors in a row before the decoder state is thrown away
constexpr double kIdleSec = 3.0;                    // no packet of the program for this long: no more silence is made for it
constexpr int kOutRate = 48000;

struct Slot {
    std::vector<uint8_t> data;
    HdcAudio::Shape shape = HdcAudio::Full;
    bool used = false, crcBad = false;
    void clear() { data.clear(); used = false; crcBad = false; shape = HdcAudio::Full; }
};

struct Prog {
    int pos = -1;                                   // play position (-1: no PDU seen yet)
    Slot slot[kSlots];
    double lastPush = -1e9;
    void clear() { pos = -1; for (auto& s : slot) s.clear(); lastPush = -1e9; }
};
} // namespace

struct HdcAudio::Impl {
    mutable std::mutex mu;
    Prog prog[kPrograms];
    std::unique_ptr<HdcDecoder> dec;                // of the program being played
    int decProg = -1;                               // which program `dec` and the output belong to
    int errRun = 0;
    double clock = -1;                              // signal time the play positions have reached
    double now = 0;
    RationalResampler rs;
    bool rsReady = false;
    std::vector<cf32> rsIn, rsOut;
    std::vector<float> pend;                        // interleaved 48 kHz output of this advanceTo()
    std::vector<int16_t> pcm;
    std::unique_ptr<AudioOut> out;
    std::function<void(const float*, size_t)> tap;
    Stats st;

    void clearAll() {
        for (auto& p : prog) p.clear();
        dec.reset(); decProg = -1; errRun = 0; clock = -1;
        rs.reset(); pend.clear();
        if (out) out->flush();
    }

    // zero or decoded 44.1 kHz stereo frames into the 48 kHz output buffer
    void emit(const int16_t* lr, int frames) {
        if (!rsReady) { rsReady = rs.configure(kHdcRate, kOutRate); }
        rsIn.resize((size_t)frames);
        for (int i = 0; i < frames; i++) rsIn[(size_t)i] = cf32(lr ? lr[2 * i] / 32768.f : 0.f, lr ? lr[2 * i + 1] / 32768.f : 0.f);
        rsOut.clear();
        rs.process(rsIn.data(), rsIn.size(), rsOut);
        for (const cf32& v : rsOut) { pend.push_back(v.real()); pend.push_back(v.imag()); }
    }
    void losePacket() {
        st.lost++;
        if (dec) dec->reset();
        errRun = 0;
        emit(nullptr, kHdcFrameSamples);
    }
    // one play step of the program that is being heard
    void playStep(Slot& s) {
        if (s.used && s.shape == Full && !s.crcBad && !s.data.empty()) {
            if ((!dec || !dec->ok()) && hdcAvailable()) dec = std::make_unique<HdcDecoder>();   // installed while running ("Look again")
            if (dec && dec->ok()) {
                const int n = dec->decode(s.data.data(), s.data.size(), pcm);
                if (n > 0) { st.decoded++; st.lastAudioSec = now; errRun = 0; emit(pcm.data(), n); return; }
                st.errors++;
                if (++errRun >= kRepeatedErrors) { dec->reset(); st.resets++; errRun = 0; }
                emit(nullptr, kHdcFrameSamples);
                return;
            }
            return;   // no library: nothing to play
        }
        if (dec && dec->ok()) losePacket();
    }
    void step(int sel) {
        for (int p = 0; p < kPrograms; p++) {
            Prog& P = prog[p];
            if (P.pos < 0) continue;
            Slot& s = P.slot[P.pos];
            if (p == sel && now - P.lastPush < kIdleSec) playStep(s);
            s.clear();
            P.pos = (P.pos + 1) % kSlots;
        }
    }
};

HdcAudio::HdcAudio() : p_(new Impl) {}
HdcAudio::~HdcAudio() = default;

void HdcAudio::align(int program, int stream, unsigned offset) {
    if (stream != 0 || program < 0 || program >= kPrograms) return;   // the enhanced streams (1) are not played, as in nrsc5
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->prog[program].pos = (int)(offset % kSlots);
}

void HdcAudio::push(int program, int stream, unsigned seq, Shape shape, bool crcOk, const uint8_t* data, size_t n) {
    if (stream != 0 || program < 0 || program >= kPrograms) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    Prog& P = p_->prog[program];
    P.lastPush = p_->now;
    Slot& s = P.slot[seq % kSlots];
    if (shape == HalfBack && s.used && s.shape == HalfFront) {   // the second half of a packet that crossed a PDU: join them
        s.shape = Full;
        if (!crcOk) s.crcBad = true;
        if (!s.crcBad) s.data.insert(s.data.end(), data, data + n); else s.data.clear();
        return;
    }
    s.used = true;
    s.shape = shape;
    s.crcBad = !crcOk;
    if (crcOk) s.data.assign(data, data + n); else s.data.clear();
}

void HdcAudio::advanceTo(double sec) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    m.now = sec;
    const int sel = program_.load();
    if (sel != m.decProg) {   // another program: its own decoder, nothing of the old one left in the queue
        m.dec.reset(); m.decProg = sel; m.errRun = 0; m.pend.clear();
        m.rs.reset();
        if (m.out) m.out->flush();
        m.st.program = sel;
    }
    if (m.clock < 0 || sec - m.clock > 5.0 || sec < m.clock) m.clock = sec;   // first call, a jump (retune, file seek): no catching up
    m.pend.clear();
    while (m.clock + kStep <= sec) { m.clock += kStep; m.step(sel); }
    if (m.pend.empty()) return;
    const size_t frames = m.pend.size() / 2;
    if (m.tap) m.tap(m.pend.data(), frames);
    if (silent_.load()) return;
    if (!m.out) { m.out = std::make_unique<AudioOut>(); m.out->start(kOutRate); m.out->setStartThreshold(kOutRate / 5); }
    m.out->setVolume(volume_.load()); m.out->setMuted(muted_.load());
    if (m.out->bufferedFrames() > kOutRate * 3 / 2) m.out->flush();   // a source that runs faster than real time: drop rather than lag
    m.out->write(m.pend.data(), (int)frames);
}

void HdcAudio::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->clearAll();
    p_->st = Stats();
    p_->st.program = program_.load();
}

void HdcAudio::setTap(std::function<void(const float*, size_t)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tap = std::move(cb);
}

HdcAudio::Stats HdcAudio::stats() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Stats s = p_->st;
    s.libFound = hdcAvailable(&s.libPath, &s.libProblem);
    s.program = program_.load();
    return s;
}

}} // namespace dect2::hdr
