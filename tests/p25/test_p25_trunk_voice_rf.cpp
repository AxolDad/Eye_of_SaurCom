// Voice on a second radio, through two real P25 receivers: radio 1 stays on the synthetic control channel (grants for TG 1201 on
// 851.20625 MHz), the follower sends the call to voice radio 0, the second receiver is "tuned" there (it starts getting the synthetic
// voice transmission), its telemetry keeps the call (voice) and ends it (the terminator), the voice radio is released, and the control
// radio is never retuned. Silent.
#include "dect2/p25/p25_gen.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25/trunk.h"
#include "dect2/p25/trunk_voice.h"
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>
using namespace dect2;
using namespace dect2::trunk;
#include "../common/test_util.h"

int main() {
    const double rate = 500000;
    P25GenConfig cc;
    cc.rate = rate; cc.snrDb = 25; cc.cfoHz = 200; cc.loop = false;
    P25GenTx ctlTx;
    ctlTx.kind = P25GenTx::Control;
    ctlTx.durSec = 8;
    cc.txs.push_back(ctlTx);
    P25Generator ctlGen(cc);
    const double voiceHz = ctlTx.idenBaseHz + 0x10 * ctlTx.idenSpacingHz;   // the grant's channel 0x1010

    P25GenConfig vc;
    vc.rate = rate; vc.snrDb = 25; vc.cfoHz = -150; vc.loop = false; vc.seed = 7;
    P25GenTx voiceTx;
    voiceTx.kind = P25GenTx::Voice;
    voiceTx.dst = ctlTx.grantTg; voiceTx.src = ctlTx.grantSrc;
    voiceTx.durSec = 2.0;
    vc.txs.push_back(voiceTx);
    P25Generator voiceGen(vc);
    P25GenConfig nc = vc;   // what the voice radio hears before it is sent anywhere: noise
    nc.txs.clear();
    P25Generator noiseGen(nc);

    P25Receiver ctlRx, voiceRx;
    ctlRx.setSilent(true); voiceRx.setSilent(true);
    ctlRx.configure(rate); voiceRx.configure(rate);

    Controller ctl;
    const double ccHz = ctlTx.idenBaseHz + (ctlTx.ctrlChannel & 0xFFF) * ctlTx.idenSpacingHz;
    ctl.setCapture(ccHz, 0);   // a narrow radio on the control channel: every call is outside its capture
    FollowConfig fc; fc.hangSec = 3.0; fc.record = false;
    ctl.follower.setConfig(fc);
    ctl.setVoiceRadios(1);

    std::set<uint64_t> seen;
    uint64_t ctlSeq = 0, voiceSeq = 0;
    std::vector<cf32> buf(25000);
    double t = 0;
    double voiceRadioHz = 0, retunedAt = -1, firstVoiceAt = -1, endedAt = -1, lastVoiceAt = -1;
    bool controlRetuned = false, hopped = false, released = false;
    uint64_t callOnRadio = 0;
    size_t tsbkWhenRetuned = 0;
    VoiceWatch watch;
    P25Telemetry vtel;
    for (double s = 0; s < 8.5; s += buf.size() / rate) {
        ctlGen.generate(buf.data(), buf.size());
        ctlRx.feed(buf.data(), buf.size());
        // the voice radio: the voice channel's transmission once it is tuned there, noise before
        if (std::fabs(voiceRadioHz - voiceHz) < 1) voiceGen.generate(buf.data(), buf.size());
        else noiseGen.generate(buf.data(), buf.size());
        voiceRx.feed(buf.data(), buf.size());
        t += buf.size() / rate;
        P25Telemetry tel;
        if (ctlRx.telemetry(tel, ctlSeq)) {
            ctlSeq = tel.seq;
            for (const auto& r : tel.tsbks) {
                if (!seen.insert(r.seq).second) continue;
                p25::Tsbk k; k.opcode = r.opcode; k.mfid = r.mfid; k.args = r.args;
                ctl.onTsbk(k, r.nac, t);
            }
        }
        if (voiceRx.telemetry(vtel, voiceSeq)) voiceSeq = vtel.seq;
        if (const uint64_t id = ctl.follower.voiceCall(0)) {
            const CallRecord* r = ctl.log.byId(id);
            const VoiceHeard h = r ? p25VoiceHeard(watch, id, r->tg, vtel.now) : VoiceHeard::Nothing;
            if (h == VoiceHeard::Voice) { ctl.onVoice(id, t); if (firstVoiceAt < 0) firstVoiceAt = t; lastVoiceAt = t; }
            else if (h == VoiceHeard::Ended) ctl.onTerminator(id, t);
        }
        ctl.tick(t);
        for (const Action& a : ctl.takeActions()) {
            if (a.kind == Action::Retune && a.radio < 0) controlRetuned = true;
            if (a.kind == Action::Retune && a.radio == 0) {
                if (retunedAt < 0) { retunedAt = t; callOnRadio = a.callId; tsbkWhenRetuned = seen.size(); }
                voiceRadioHz = a.freqHz;
            }
            if (a.kind == Action::ReleaseRadio && a.radio == 0 && a.callId == callOnRadio && endedAt < 0) { endedAt = t; released = true; }
        }
        hopped |= ctl.follower.state() == FollowState::Hopped || ctl.follower.state() == FollowState::Returning;
    }
    const CallRecord* call = callOnRadio ? ctl.log.byId(callOnRadio) : nullptr;
    printf("  %zu TSBKs (%zu by the retune), retune at %.2f s to %.5f MHz, voice %.2f..%.2f s, released at %.2f s, state %s\n", seen.size(), tsbkWhenRetuned,
           retunedAt, voiceRadioHz / 1e6, firstVoiceAt, lastVoiceAt, endedAt, followStateName(ctl.follower.state()));
    CHECK(retunedAt > 0 && std::fabs(voiceRadioHz - voiceHz) < 1, "the grant sent voice radio 0 to %.5f MHz", voiceHz / 1e6);
    CHECK(call && call->tg == ctlTx.grantTg && call->followed, "the call is followed on the voice radio");
    CHECK(firstVoiceAt > retunedAt, "the voice radio heard the call's voice");
    CHECK(released && endedAt > lastVoiceAt && endedAt - lastVoiceAt < fc.hangSec - 0.5, "the terminator ended the call (%.2f s after the last voice, hang time %.1f s)", endedAt - lastVoiceAt, fc.hangSec);
    CHECK(!controlRetuned && !hopped, "the control radio was never retuned");
    CHECK(seen.size() > tsbkWhenRetuned + 10, "the control channel kept decoding during the call (%zu -> %zu TSBKs)", tsbkWhenRetuned, seen.size());
    return testSummary("p25 trunk voice rf");
}
