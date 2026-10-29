// A P25 Phase 2 call followed on a second radio, through a real P25 receiver: the Phase 1 control channel announces a TDMA channel
// identifier (IDEN_UP_TDMA, two slots) and grants TG 2402 on slot 1 of a Phase 2 carrier. The follower sends voice radio 0 there with the
// slot; the voice radio's P25Receiver is switched to Phase 2 on that slot (what the app does with the Retune) and gets the synthetic Phase 2
// carrier, which carries a call on each slot. The vocoder is handed exactly slot 1's AMBE+2 frames in order (none of slot 0's), the
// receiver's telemetry keeps the call (voice) and ends it (MAC_END_PTT), the radio is released and the control radio is never retuned.
// The same again with a call encrypted in its MAC_PTT: logged as encrypted, nothing handed to the vocoder. Silent.
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25/p25p2.h"
#include "dect2/p25/trunk.h"
#include "dect2/p25/trunk_voice.h"
#include <cmath>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::trunk;
#include "../common/test_util.h"

namespace {

struct Result {
    bool retuned = false, controlRetuned = false, released = false, p2Asked = false;
    int slot = -1;
    double retuneHz = 0, firstVoice = -1, lastVoice = -1, endedAt = -1;
    std::vector<std::vector<char>> frames;
    int clearFrames = 0;
    uint64_t callId = 0;
    std::string note;
    bool encrypted = false;
};

Result runCall(int algId) {
    const double rate = 500000, offset = -50000;
    const int wacn = 0xBEE00, sys = 0x3A1, nac = 0x293;
    const double ccHz = 851006250 + 1 * 12500.0, p2Base = 769000000, p2Spacing = 12500;
    const int p2Ch = 40;   // the Phase 2 carrier's channel; with two slots a grant's channel number is carrier * 2 + slot
    const double p2Hz = p2Base + p2Ch * p2Spacing;

    Controller ctl;
    ctl.setCapture(ccHz, 0);
    FollowConfig fc; fc.hangSec = 2.0; fc.record = false;
    ctl.follower.setConfig(fc);
    ctl.setVoiceRadios(1);

    // the voice carrier: slot 0 carries TG 1201 all along, slot 1 the granted call on TG 2402
    p25p2::Generator gen(wacn, sys, nac);
    p25p2::GenCall c0; c0.chan = 0; c0.startSf = 1; c0.superframes = 14; c0.tg = 1201; c0.src = 100;
    p25p2::GenCall c1; c1.chan = 1; c1.startSf = 3; c1.superframes = 6; c1.tg = 2402; c1.src = 7654321; c1.algId = algId; c1.keyId = algId == 0x80 ? 0 : 9;
    gen.add(c0); gen.add(c1);
    std::vector<uint8_t> dib;
    for (int s = 0; s < 16; s++) { auto v = gen.superframe(s); dib.insert(dib.end(), v.begin(), v.end()); }
    std::vector<cf32> voiceIq = p25p2::modulate(dib, rate, offset + 180, 0.2f);
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 1.f);
    const float sd = (float)std::sqrt(0.04 / std::pow(10.0, 25 / 10.0) * rate / 12500.0 / 2);
    for (cf32& v : voiceIq) v += cf32(sd * nd(rng), sd * nd(rng));

    P25Receiver vrx;
    vrx.setSilent(true);
    vrx.configure(rate);
    vrx.setSignalOffset(offset);
    Result R;
    vrx.setPhase2VoiceTap([&](int, const char fr[4][24], bool clear) { R.frames.emplace_back(&fr[0][0], &fr[0][0] + 96); R.clearFrames += clear; });

    // the control channel's messages, as the Phase 1 receiver hands them on (the RF of the control channel is test_p25_trunk_voice_rf's)
    double t = 0;
    auto tsbk = [&](const p25::Tsbk& k) { ctl.onTsbk(k, nac, t); };
    VoiceWatch watch;
    P25Telemetry vt;
    uint64_t vseq = 0;
    size_t pos = 0;
    const size_t blk = 12500;   // 25 ms
    bool voiceRadioThere = false;
    for (t = 0; t < 7.5; t += blk / rate) {
        // every 100 ms the site, the identifiers and (from 0.3 s, while the call runs) the grant repeat
        if (std::fmod(t, 0.1) < blk / rate) {
            tsbk(gen::netSts(wacn, sys, 0x1001, 0x70));
            tsbk(gen::rfssSts(sys, 1, 5, 0x1001, 0x70));
            tsbk(gen::idenUp(1, 851006250, 12500, -45000000, 12500));
            tsbk(gen::idenUpTdma(2, 3, p2Base, p2Spacing, 30000000));
            if (t > 0.3 && t < 2.3) tsbk(gen::grpVGrant(0, (2 << 12) | (p2Ch * 2 + 1), 2402, 7654321));
        }
        // the voice radio hears the Phase 2 carrier once it is there (noise before)
        std::vector<cf32> b(blk);
        if (voiceRadioThere && pos < voiceIq.size()) {
            const size_t n = std::min(blk, voiceIq.size() - pos);
            std::copy(voiceIq.begin() + (long)pos, voiceIq.begin() + (long)(pos + n), b.begin());
            pos += n;
        } else for (cf32& v : b) v = cf32(sd * nd(rng), sd * nd(rng));
        vrx.feed(b.data(), b.size());
        if (vrx.telemetry(vt, vseq)) vseq = vt.seq;
        if (const uint64_t id = ctl.follower.voiceCall(0)) {
            const CallRecord* r = ctl.log.byId(id);
            const VoiceHeard h = r ? p25VoiceHeard(watch, id, r->tg, vt.now, vt.seq) : VoiceHeard::Nothing;
            if (h == VoiceHeard::Voice) { ctl.onVoice(id, t); if (R.firstVoice < 0) R.firstVoice = t; R.lastVoice = t; }
            else if (h == VoiceHeard::Ended) ctl.onTerminator(id, t);
            if (vt.now.active && vt.now.encrypted && r) R.encrypted = true;
        }
        ctl.tick(t);
        for (const Action& a : ctl.takeActions()) {
            if (a.kind == Action::Retune && a.radio < 0) R.controlRetuned = true;
            if (a.kind == Action::Retune && a.radio == 0) {
                R.retuned = true; R.retuneHz = a.freqHz; R.slot = a.slot; R.callId = a.callId;
                R.p2Asked = a.p25p2;
                vrx.setPhase2(a.p25p2, a.slot, ctl.sys.site().wacn, ctl.sys.site().sysId, ctl.sys.site().nac);   // what the app does
                vrx.setSignalOffset(offset);
                voiceRadioThere = std::fabs(a.freqHz - p2Hz) < 1;
            }
            if (a.kind == Action::ReleaseRadio && a.radio == 0 && a.callId == R.callId && R.endedAt < 0) { R.endedAt = t; R.released = true; }
        }
    }
    if (const CallRecord* r = ctl.log.byId(R.callId)) R.note = r->note;
    return R;
}

int matching(const std::vector<std::vector<char>>& got, int chan) {
    auto ref = [&](int n) { uint32_t c[4]; p25p2::Generator::voiceWords(chan, (uint32_t)n, c); char fr[4][24]; p25p2::voiceFr(c, fr); return std::vector<char>(&fr[0][0], &fr[0][0] + 96); };
    int start = -1;
    for (int n = 0; n < 2000 && start < 0 && !got.empty(); n++) if (ref(n) == got[0]) start = n;
    if (start < 0) return 0;
    int ok = 0;
    for (size_t i = 0; i < got.size(); i++) ok += ref(start + (int)i) == got[i];
    return ok;
}

} // namespace

int main() {
    {
        const Result R = runCall(0x80);
        const int ok1 = matching(R.frames, 1), ok0 = matching(R.frames, 0);
        printf("  clear call: retune %d to %.5f MHz slot %d (Phase 2 %d), voice %.2f..%.2f s, released at %.2f s, %zu frames to the vocoder (%d of slot 1's in order, %d of slot 0's)\n",
               R.retuned, R.retuneHz / 1e6, R.slot, R.p2Asked, R.firstVoice, R.lastVoice, R.endedAt, R.frames.size(), ok1, ok0);
        CHECK(R.retuned && R.slot == 1 && R.p2Asked && std::fabs(R.retuneHz - (769000000 + 40 * 12500.0)) < 1, "the Phase 2 grant sent voice radio 0 to the carrier, slot 1");
        CHECK(R.frames.size() >= 18 * 5 && ok1 == (int)R.frames.size() && ok0 == 0, "the vocoder got slot 1's frames only, in order: %d of %zu (slot 0: %d)", ok1, R.frames.size(), ok0);
        CHECK(R.clearFrames == (int)R.frames.size(), "every frame handed on as clear");
        CHECK(R.firstVoice > 0 && R.released && R.endedAt > R.lastVoice && R.endedAt - R.lastVoice < 1.5, "MAC_END_PTT ended the call (%.2f s after the last voice)", R.endedAt - R.lastVoice);
        CHECK(!R.controlRetuned, "the control radio was never retuned");
    }
    {
        const Result R = runCall(0x84);
        printf("  encrypted call: %zu frames seen, %d handed on as clear, encrypted %d, released %d\n", R.frames.size(), R.clearFrames, R.encrypted, R.released);
        CHECK(R.retuned && R.encrypted, "the MAC_PTT's ALGID marks the call encrypted");
        CHECK(R.clearFrames == 0, "no encrypted frame is handed on as clear (%d)", R.clearFrames);
    }
    return testSummary("p25 phase 2 rf");
}
