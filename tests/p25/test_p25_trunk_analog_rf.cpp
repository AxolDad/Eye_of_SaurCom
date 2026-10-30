// Analog trunked calls followed on a second radio, through the real receivers: for EDACS, LTR and MPT 1327 a control receiver decodes a
// generated control signal (EDACS frames with a group call on LCN 2; LTR words saying talkgroup 0-03-017 talks on repeater 5; an MPT 1327
// control channel with a GTC to channel 17), the follower sends voice radio 0 to the channel from the user's table, the voice radio's own
// receiver of that mode is put there (it then gets the generated traffic channel: FM speech, a 1 kHz tone, with the system's own signalling),
// plays the tone, keeps the call while the carrier is up and ends it on the channel's end signalling (EDACS: the end-of-transmission dotting; LTR: the
// turn-off words; MPT 1327: CLEAR after SYNT). The control radio is never retuned. Silent.
#include "dect2/edacs/edacs_proto.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include "dect2/ltr/ltr_proto.h"
#include "dect2/ltr/ltr_rx.h"
#include "dect2/ltr/ltr_tel.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/mpt1327/mpt1327_gen.h"
#include "dect2/p25/trunk.h"
#include "dect2/p25/trunk_voice.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::trunk;
#include "../common/test_util.h"

namespace {

std::mt19937 rng(23);
std::normal_distribution<float> nd(0.f, 1.f);

double tonePart(const std::vector<float>& a) {   // the share of the sound's power at 1 kHz
    if (a.size() < 9600) return 0;
    double c = 0, s = 0, e = 0;
    const size_t a0 = a.size() / 4, a1 = a.size() * 3 / 4;
    for (size_t i = a0; i < a1; i++) { const double ph = 2 * 3.14159265358979323846 * 1000.0 * (double)i / 48000.0; c += a[i] * std::cos(ph); s += a[i] * std::sin(ph); e += (double)a[i] * a[i]; }
    return e > 0 ? 2 * (c * c + s * s) / ((double)(a1 - a0) * e) : 0;
}

struct Outcome {
    bool retuned = false, controlRetuned = false, released = false, followed = false;
    double retuneHz = 0, firstVoice = -1, lastVoice = -1, endedAt = -1;
    size_t audio = 0;
    double tone = 0;
    std::string label;
};

void report(const char* sys, const Outcome& o, double wantHz) {
    printf("  %s: retune %d to %.4f MHz, voice %.2f..%.2f s, released at %.2f s, %zu sound samples (%.0f %% at 1 kHz), call %s\n", sys, o.retuned, o.retuneHz / 1e6,
           o.firstVoice, o.lastVoice, o.endedAt, o.audio, 100 * o.tone, o.label.c_str());
    CHECK(o.retuned && std::fabs(o.retuneHz - wantHz) < 1 && o.followed, "%s: the call went to voice radio 0 on %.4f MHz", sys, wantHz / 1e6);
    CHECK(o.firstVoice > 0 && o.audio > 48000 && o.tone > 0.8, "%s: the voice radio played the call (%.0f %% at 1 kHz)", sys, 100 * o.tone);
    CHECK(o.released && o.endedAt > o.lastVoice - 0.3 && o.endedAt - o.lastVoice < 1.0, "%s: the end signalling ended the call (%.2f s after the last voice, hang time 3 s)", sys, o.endedAt - o.lastVoice);
    CHECK(!o.controlRetuned, "%s: the control radio was never retuned", sys);
}

// The loop the app runs: control receiver -> controller, follower actions -> the voice radio, the voice radio's telemetry -> the controller
struct Loop {
    double rate, offset, t = 0;
    Controller ctl;
    Outcome o;
    VoiceWatch watch;
    uint64_t callId = 0;
    std::function<void(cf32*, size_t)> control, voiceSig;   // the signals the two radios get
    std::function<void(const cf32*, size_t)> feedControl, feedVoice;
    std::function<void(double)> pollControl;                // control telemetry into ctl
    std::function<VoiceHeard(const CallRecord&)> heard;     // the voice radio's telemetry
    std::function<void(double)> tuneVoice;                  // the voice radio goes to a channel
    std::function<void(bool)> voiceOn;                      // its sound on / off
    void run(double secs) {
        FollowConfig fc; fc.hangSec = 3.0; fc.record = false;
        ctl.follower.setConfig(fc);
        ctl.setVoiceRadios(1);
        std::vector<cf32> b(12000);
        for (t = 0; t < secs; t += (double)b.size() / rate) {
            control(b.data(), b.size()); feedControl(b.data(), b.size());
            voiceSig(b.data(), b.size()); feedVoice(b.data(), b.size());
            pollControl(t);
            if (const uint64_t id = ctl.follower.voiceCall(0)) {
                if (const CallRecord* r = ctl.log.byId(id)) {
                    const VoiceHeard h = heard(*r);
                    if (h == VoiceHeard::Voice) { ctl.onVoice(id, t); if (o.firstVoice < 0) o.firstVoice = t; o.lastVoice = t; }
                    else if (h == VoiceHeard::Ended) ctl.onTerminator(id, t);
                }
            }
            ctl.tick(t);
            for (const Action& a : ctl.takeActions()) {
                if (a.kind == Action::Retune && a.radio < 0) o.controlRetuned = true;
                if (a.kind == Action::Retune && a.radio == 0) {
                    if (!o.retuned) { o.retuned = true; o.retuneHz = a.freqHz; callId = a.callId; }
                    tuneVoice(a.freqHz);
                    voiceOn(true);
                }
                if (a.kind == Action::ReleaseRadio && a.radio == 0) { voiceOn(false); if (a.callId == callId && o.endedAt < 0) { o.endedAt = t; o.released = true; } }
            }
        }
        if (const CallRecord* r = ctl.log.byId(callId)) { o.followed = r->followed; o.label = r->label; }
    }
};

void noise(cf32* b, size_t n, float sd) { for (size_t i = 0; i < n; i++) b[i] = cf32(sd * nd(rng), sd * nd(rng)); }

} // namespace

int main() {
    // ---------------------------------------------------------------- EDACS
    {
        const double rate = 480000, off = -100000, lcn2 = 851.2625e6;
        std::vector<uint8_t> bits;
        for (int i = 0; i < 300; i++) bits.push_back((uint8_t)(i * 7 % 3 == 0));
        for (int k = 0; k < 400; k++) {   // 12 s of frames: site ID, then the group call on LCN 2 for 1 s
            edacs::Message a;
            a.kind = edacs::Message::SiteId; a.site = 4; a.ccLcn = 1;
            if (k >= 20 && k < 55) { a = edacs::Message(); a.kind = edacs::Message::GroupVoice; a.lcn = 2; a.group = 0x2A5; a.source = 0x123; }
            uint32_t m1, m2;
            edacs::build(a, edacs::Mode::Standard, m1, m2);
            const auto f = edacs::encodeFrame(m1, m2);
            bits.insert(bits.end(), f.begin(), f.end());
        }
        const std::vector<cf32> ctlIq = edacs::modulate(bits, rate, off, 2400.0, 0.4f);
        const std::vector<cf32> work = edacs::workingChannel(rate, off, 3.0, 1000.0, 96, 0.4f);
        size_t cp = 0, vp = 0;
        bool there = false;
        EdacsReceiver crx, vrx;
        crx.configure(rate); crx.setSignalOffset(off); crx.setCenterHz(851.0125e6 - off); crx.setSilent(true);
        vrx.configure(rate); vrx.setSignalOffset(off); vrx.setCenterHz(851.0125e6 - off); vrx.setSilent(true);
        std::vector<float> audio;
        vrx.setAudioTap([&](const float* x, size_t n) { audio.insert(audio.end(), x, x + n); });
        Loop L;
        L.rate = rate; L.offset = off;
        L.ctl.edacs.lcns.parse("1,851.0125\n2,851.2625\n3,851.5125\n", 31);
        L.ctl.setCapture(851.0125e6, 0);
        uint64_t cseq = 0, lastMsg = 0, vseq = 0;
        EdacsTelemetry vt;
        L.control = [&](cf32* b, size_t n) { for (size_t i = 0; i < n; i++) b[i] = (cp < ctlIq.size() ? ctlIq[cp++] : cf32(0, 0)) + cf32(0.01f * nd(rng), 0.01f * nd(rng)); };
        L.voiceSig = [&](cf32* b, size_t n) { if (there) for (size_t i = 0; i < n; i++) b[i] = (vp < work.size() ? work[vp++] : cf32(0, 0)) + cf32(0.01f * nd(rng), 0.01f * nd(rng)); else noise(b, n, 0.2f); };
        L.feedControl = [&](const cf32* b, size_t n) { crx.feed(b, n); };
        L.feedVoice = [&](const cf32* b, size_t n) { vrx.feed(b, n); if (vrx.telemetry(vt, vseq)) vseq = vt.seq; };
        L.pollControl = [&](double t) {
            EdacsTelemetry ct;
            if (crx.telemetry(ct, cseq)) { cseq = ct.seq; for (const EdacsMsgRec& m : ct.msgs) if (m.seq > lastMsg) { lastMsg = m.seq; L.ctl.onEdacs(m.m1, m.m2, t); } }
        };
        L.heard = [&](const CallRecord& r) { return analogVoiceHeard(L.watch, r.id, std::fabs(vt.channelHz - r.freqHz) < 2000, vt.voice.carrier, vt.eot, true, vt.seq); };
        L.tuneVoice = [&](double hz) { vrx.setCenterHz(hz - off); there = std::fabs(hz - lcn2) < 1; vp = 0; vrx.reset(); };
        L.voiceOn = [&](bool on) { vrx.setVoice(on); };
        L.run(8.0);
        L.o.audio = audio.size(); L.o.tone = tonePart(audio);
        report("EDACS", L.o, lcn2);
        CHECK(L.ctl.edacs.site() == 4, "EDACS: the site ID from the control channel");
    }
    // ---------------------------------------------------------------- LTR
    {
        const double rate = 240000, off = -50000, rep1 = 460.0125e6, rep5 = 460.1125e6;
        // the monitoring receiver is on repeater 1; its words say 0-03-017 talks on repeater 5, then repeater 1 gets busy with another call
        // (0-01-050 on repeater 1), which finds the one voice radio taken
        std::vector<uint8_t> bits;
        for (int i = 0; i < 40; i++) bits.push_back((uint8_t)(i & 1));
        for (int k = 0; k < 60; k++) {
            ltr::Word w; w.area = 0; w.freeCh = 2;
            if (k % 2 == 1 || k > 13) { w.gotoCh = 1; w.home = 1; w.id = 50; } else { w.gotoCh = 5; w.home = 3; w.id = 17; }
            const auto e = ltr::encodeWord(w);
            bits.insert(bits.end(), e.begin(), e.end());
            for (int i = 0; i < 12; i++) bits.push_back((uint8_t)((i * 3 + k) & 1));
        }
        const std::vector<cf32> ctlIq = ltr::modulate(bits, rate, off, 500.0, 0.4f, 700.0, 2000.0);
        const std::vector<cf32> work = ltr::transmission(rate, off, 0, 5, 3, 17, 3.0, 1000.0, 4, 0.4f);
        size_t cp = 0, vp = 0;
        bool there = false;
        LtrReceiver crx, vrx;
        crx.configure(rate); crx.setSignalOffset(off); crx.setCenterHz(rep1 - off); crx.setSilent(true);
        vrx.configure(rate); vrx.setSignalOffset(off); vrx.setCenterHz(rep1 - off); vrx.setSilent(true);
        std::vector<float> audio;
        vrx.setAudioTap([&](const float* x, size_t n) { audio.insert(audio.end(), x, x + n); });
        Loop L;
        L.rate = rate; L.offset = off;
        L.ctl.ltr.repeaters.parse("1,460.0125\n2,460.0375\n3,460.0625\n4,460.0875\n5,460.1125\n", 20);
        L.ctl.setCapture(rep1, 0);
        uint64_t cseq = 0, lastW = 0, vseq = 0;
        LtrTelemetry vt;
        L.control = [&](cf32* b, size_t n) { for (size_t i = 0; i < n; i++) b[i] = (cp < ctlIq.size() ? ctlIq[cp++] : cf32(0, 0)) + cf32(0.01f * nd(rng), 0.01f * nd(rng)); };
        L.voiceSig = [&](cf32* b, size_t n) { if (there) for (size_t i = 0; i < n; i++) b[i] = (vp < work.size() ? work[vp++] : cf32(0, 0)) + cf32(0.01f * nd(rng), 0.01f * nd(rng)); else noise(b, n, 0.2f); };
        L.feedControl = [&](const cf32* b, size_t n) { crx.feed(b, n); };
        L.feedVoice = [&](const cf32* b, size_t n) { vrx.feed(b, n); if (vrx.telemetry(vt, vseq)) vseq = vt.seq; };
        L.pollControl = [&](double t) {
            LtrTelemetry ct;
            if (crx.telemetry(ct, cseq)) { cseq = ct.seq; for (const LtrWordRec& w : ct.words) if (w.seq > lastW) { lastW = w.seq; L.ctl.onLtr(w.area, w.gotoCh, w.home, w.id, w.freeCh, t); } }
        };
        L.heard = [&](const CallRecord& r) {
            const bool ours = ltr::tgNumber(vt.turnOffArea, vt.turnOffHome, vt.turnOffId) == r.tg;
            return analogVoiceHeard(L.watch, r.id, std::fabs(vt.channelHz - r.freqHz) < 2000, vt.voice.carrier, vt.turnOffs, ours, vt.seq);
        };
        L.tuneVoice = [&](double hz) { vrx.setCenterHz(hz - off); there = std::fabs(hz - rep5) < 1; vp = 0; vrx.reset(); };
        L.voiceOn = [&](bool on) { vrx.setVoice(on); };
        L.run(9.0);
        L.o.audio = audio.size(); L.o.tone = tonePart(audio);
        report("LTR", L.o, rep5);
        CHECK(L.o.label == "0-03-017", "LTR: the call is 0-03-017 (%s)", L.o.label.c_str());
    }
    // ---------------------------------------------------------------- MPT 1327
    {
        const double rate = 240000, off = -50000, ccHz = 165.0e6 + 40 * 12500, ch17 = 165.0e6 + 17 * 12500;
        using namespace mpt1327;
        std::vector<uint64_t> addr;
        for (int k = 0; k < 60; k++) addr.push_back(k >= 8 && k < 14 ? makeGtc(5, 1234, 0, 17, 2003) : makeAloha(0, 0, 0, 0));
        std::vector<float> ctlAud(4800, 0.f);
        const auto cab = ffskAudio(controlChannelBits(20001, addr), rate, false, 0.5f);
        ctlAud.insert(ctlAud.end(), cab.begin(), cab.end());
        const std::vector<std::complex<float>> ctlIq = fmModulate(ctlAud, rate, 3000.0, off);
        std::vector<float> traffic;
        for (size_t i = 0; i < (size_t)(3.0 * rate); i++) traffic.push_back(0.8f * (float)std::sin(2 * 3.14159265358979323846 * 1000.0 * (double)i / rate));
        const auto clr = ffskAudio(trafficChannelBits({makeClear(17, 0), makeClear(17, 0)}), rate, false, 0.5f);
        traffic.insert(traffic.end(), clr.begin(), clr.end());
        const std::vector<std::complex<float>> work = fmModulate(traffic, rate, 3000.0, off);
        size_t cp = 0, vp = 0;
        bool there = false;
        MptReceiver crx, vrx;
        crx.configure(rate); crx.setSignalOffset(off); crx.setCenterHz(ccHz - off); crx.setSilent(true);
        vrx.configure(rate); vrx.setSignalOffset(off); vrx.setCenterHz(ccHz - off); vrx.setSilent(true);
        std::vector<float> audio;
        vrx.setAudioTap([&](const float* x, size_t n) { audio.insert(audio.end(), x, x + n); });
        Loop L;
        L.rate = rate; L.offset = off;
        L.ctl.mpt.channels.setFormula(165.0e6, 12500);
        L.ctl.setCapture(ccHz, 0);
        uint64_t cseq = 0, lastM = 0, vseq = 0;
        MptTelemetry vt;
        L.control = [&](cf32* b, size_t n) { for (size_t i = 0; i < n; i++) b[i] = (cp < ctlIq.size() ? ctlIq[cp++] : cf32(0, 0)) + cf32(0.01f * nd(rng), 0.01f * nd(rng)); };
        L.voiceSig = [&](cf32* b, size_t n) { if (there) for (size_t i = 0; i < n; i++) b[i] = (vp < work.size() ? work[vp++] : cf32(0, 0)) + cf32(0.01f * nd(rng), 0.01f * nd(rng)); else noise(b, n, 0.2f); };
        L.feedControl = [&](const cf32* b, size_t n) { crx.feed(b, n); };
        L.feedVoice = [&](const cf32* b, size_t n) { vrx.feed(b, n); if (vrx.telemetry(vt, vseq)) vseq = vt.seq; };
        L.pollControl = [&](double t) {
            MptTelemetry ct;
            if (crx.telemetry(ct, cseq)) {
                cseq = ct.seq;
                for (const MptMsgRec& m : ct.msgs) if (m.seq > lastM) { lastM = m.seq; L.ctl.onMpt(m.type, m.calledPfix, m.calledIdent, m.callerIdent, m.channel, m.dataCall, m.traffic, t); }
            }
        };
        L.heard = [&](const CallRecord& r) { return analogVoiceHeard(L.watch, r.id, std::fabs(vt.channelHz - r.freqHz) < 2000, vt.voice.carrier, vt.clears, true, vt.seq); };
        L.tuneVoice = [&](double hz) { vrx.setCenterHz(hz - off); there = std::fabs(hz - ch17) < 1; vp = 0; vrx.reset(); };
        L.voiceOn = [&](bool on) { vrx.setVoice(on); };
        L.run(9.0);
        L.o.audio = audio.size(); L.o.tone = tonePart(audio);
        report("MPT1327", L.o, ch17);
        CHECK(L.o.label == "5/1234", "MPT1327: the call is 5/1234 (%s)", L.o.label.c_str());
        CHECK(vt.clears >= 1, "MPT1327: the CLEAR was decoded on the traffic channel (%llu)", (unsigned long long)vt.clears);
    }
    return testSummary("p25 trunk analog rf");
}
