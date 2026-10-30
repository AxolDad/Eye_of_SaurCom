// Voice radios as plain logic: what a voice radio hears of its call (P25, DMR, NXDN telemetry; fresh reports only; a DMR fade does not end
// the call), the follower's actions around voice radios (order on preemption, a moved call, reset, shrinking, following switched off), and
// the rules the app applies to linked receivers (who feeds the follower, when a voice radio is unlinked, who may share a mode).
#include "dect2/p25/trunk.h"
#include "dect2/p25/trunk_voice.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::trunk;
#include "../common/test_util.h"

static p25::Tsbk wire(const p25::Tsbk& t) {
    uint8_t b[96];
    p25::tsbkBits(t, b);
    p25::Tsbk r;
    if (!p25::tsbkParse(b, r)) { printf("FAIL: tsbk did not survive its CRC\n"); fails++; }
    return r;
}
static void feed(Controller& c, const p25::Tsbk& t, double now) { c.onTsbk(wire(t), 0x293, now); }
static void setupSite(Controller& c) {
    feed(c, gen::idenUp(1, 851006250, 12500, -45000000, 12500), 0);
    feed(c, gen::netSts(0xBEE00, 0x3A1, 0x1001, 0x70), 0);
    feed(c, gen::rfssSts(0x3A1, 1, 5, 0x1001, 0x70), 0);
    feed(c, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
    c.setCapture(851006250 + 1 * 12500.0, 0);
}
static int indexOf(const std::vector<Action>& a, Action::Kind k, int radio) {
    for (size_t i = 0; i < a.size(); i++) if (a[i].kind == k && a[i].radio == radio) return (int)i;
    return -1;
}

int main() {
    // ---- P25: fresh reports only; a terminator counts after this call's own voice
    {
        VoiceWatch w;
        P25Call n;
        n.kind = 0; n.dst = 1201; n.active = false; n.endSeen = true;   // what the receiver shows of the previous call on this channel
        CHECK(p25VoiceHeard(w, 7, 1201, n, 10) == VoiceHeard::Nothing, "a terminator before this call's voice does not end it");
        n.active = true; n.endSeen = false;
        CHECK(p25VoiceHeard(w, 7, 1201, n, 11) == VoiceHeard::Voice, "voice");
        CHECK(p25VoiceHeard(w, 7, 1201, n, 11) == VoiceHeard::Nothing, "the same report again is not new voice");
        for (int i = 0; i < 50; i++) CHECK(p25VoiceHeard(w, 7, 1201, n, 11) == VoiceHeard::Nothing, "frozen telemetry keeps nothing alive");
        n.active = false; n.endSeen = true;
        CHECK(p25VoiceHeard(w, 7, 1201, n, 12) == VoiceHeard::Ended, "its terminator ends it");
        // the next call on the same radio: the old call's terminator in the report does not end it
        CHECK(p25VoiceHeard(w, 8, 1201, n, 13) == VoiceHeard::Nothing, "new call, old terminator");
        n.dst = 999; n.active = true; n.endSeen = false;
        CHECK(p25VoiceHeard(w, 8, 1201, n, 14) == VoiceHeard::Nothing, "another talkgroup's voice is not ours");
        n.dst = -1;
        CHECK(p25VoiceHeard(w, 8, 1201, n, 15) == VoiceHeard::Voice && !w.heard, "voice before the talkgroup is decoded keeps the call, not yet heard");
        n.active = false; n.endSeen = true;
        CHECK(p25VoiceHeard(w, 8, 1201, n, 16) == VoiceHeard::Nothing, "nor ends it");
    }
    // ---- NXDN: the same rules
    {
        VoiceWatch w;
        NxdnCall n;
        n.kind = 0; n.dst = 77; n.active = false; n.endSeen = true;
        CHECK(nxdnVoiceHeard(w, 3, 77, n, 1) == VoiceHeard::Nothing, "nxdn: old TX_REL does not end the new call");
        n.active = true; n.endSeen = false;
        CHECK(nxdnVoiceHeard(w, 3, 77, n, 2) == VoiceHeard::Voice, "nxdn voice");
        CHECK(nxdnVoiceHeard(w, 3, 77, n, 2) == VoiceHeard::Nothing, "nxdn stale report");
        n.kind = 1;
        CHECK(nxdnVoiceHeard(w, 3, 77, n, 3) == VoiceHeard::Nothing, "nxdn status message is not voice");
        n.kind = 0; n.active = false; n.endSeen = false;
        CHECK(nxdnVoiceHeard(w, 3, 77, n, 4) == VoiceHeard::Nothing, "nxdn signal lost: no end (hang time ends it)");
        n.endSeen = true;
        CHECK(nxdnVoiceHeard(w, 3, 77, n, 5) == VoiceHeard::Ended, "nxdn TX_REL ends it");
    }
    // ---- DMR: Terminator with LC ends the call; a fade or a data burst does not
    {
        DmrSlot s[2];
        VoiceWatch w;
        s[1].active = true; s[1].state = 1; s[1].inCall = true; s[1].dst = 101; s[1].lastBurst = "Voice C";
        CHECK(dmrVoiceHeard(w, 5, 101, 1, s, 1) == VoiceHeard::Voice && w.heard, "dmr voice on slot 2");
        CHECK(dmrVoiceHeard(w, 5, 101, 0, s, 2) == VoiceHeard::Nothing, "dmr: the other slot is not ours");
        // a fade: the track drops, the call flag clears, the talkgroup goes
        s[1].active = false; s[1].inCall = false; s[1].dst = 0; s[1].state = 0;
        CHECK(dmrVoiceHeard(w, 5, 101, 1, s, 3) == VoiceHeard::Nothing, "dmr fade does not end the call");
        // a data burst on the slot ends the decoder's call too
        s[1].active = true; s[1].state = 2; s[1].lastBurst = "Data header: x";
        CHECK(dmrVoiceHeard(w, 5, 101, 1, s, 4) == VoiceHeard::Nothing, "dmr data burst does not end the call");
        s[1].state = 1; s[1].inCall = true; s[1].dst = 101; s[1].lastBurst = "Voice A";
        CHECK(dmrVoiceHeard(w, 5, 101, 1, s, 5) == VoiceHeard::Voice, "dmr voice again");
        s[1].inCall = false; s[1].state = 3; s[1].lastBurst = "Terminator";
        CHECK(dmrVoiceHeard(w, 5, 101, 1, s, 6) == VoiceHeard::Nothing, "dmr: only the exact burst name counts");
        s[1].lastBurst = "Terminator with LC";
        CHECK(dmrVoiceHeard(w, 5, 101, 1, s, 7) == VoiceHeard::Ended, "dmr Terminator with LC ends it");
        // a new call: the old terminator in the report does not end it, and voice of an undecoded talkgroup does not count as heard
        CHECK(dmrVoiceHeard(w, 6, 101, 1, s, 8) == VoiceHeard::Nothing, "dmr new call, old terminator");
        s[1].inCall = true; s[1].state = 1; s[1].dst = 0; s[1].lastBurst = "Voice B";
        CHECK(dmrVoiceHeard(w, 6, 101, 1, s, 9) == VoiceHeard::Voice && !w.heard, "dmr late entry keeps the call");
        s[1].inCall = false; s[1].lastBurst = "Terminator with LC";
        CHECK(dmrVoiceHeard(w, 6, 101, 1, s, 10) == VoiceHeard::Nothing, "dmr: no end before the call's own voice");
    }
    // ---- DMR fade through the follower: the call stays until the hang time, not ended early
    {
        Controller c;
        c.dmr.setChannel(5, 451.0125e6);
        c.setCapture(451.5e6, 0);
        c.setVoiceRadios(1);
        FollowConfig fc; fc.hangSec = 2.0;
        c.follower.setConfig(fc);
        c.onControlSeen(0.5);
        uint8_t d[8] = {0};
        d[1] = 0x58; d[4] = 101;   // channel 5, slot bit set (the layout of test_p25_trunk), talkgroup 101
        c.onDmrCsbk(0x31, 0, d, 1.0);
        c.takeActions();
        const uint64_t id = c.follower.voiceCall(0);
        CHECK(id != 0, "dmr call on voice radio 0");
        const CallRecord* r = c.log.byId(id);
        const int slot = r ? r->slot : 0;
        DmrSlot s[2];
        VoiceWatch w;
        uint64_t seq = 1;
        double t = 1.0;
        for (; t < 3.0; t += 0.25) {   // voice
            s[slot].active = true; s[slot].state = 1; s[slot].inCall = true; s[slot].dst = 101; s[slot].lastBurst = "Voice A";
            if (dmrVoiceHeard(w, id, 101, slot, s, ++seq) == VoiceHeard::Voice) c.onVoice(id, t);
            c.onControlSeen(t); c.tick(t);
        }
        s[slot] = DmrSlot();   // a fade at 3.0 s: the decoder ends its call
        bool endedEarly = false;
        for (; t < 4.5; t += 0.25) {
            const VoiceHeard h = dmrVoiceHeard(w, id, 101, slot, s, ++seq);
            if (h == VoiceHeard::Ended) c.onTerminator(id, t);
            c.onControlSeen(t); c.tick(t);
            if (c.follower.voiceCall(0) != id) endedEarly = true;
        }
        CHECK(!endedEarly, "a fade does not end the dmr call before the hang time");
        for (; t < 6.0; t += 0.25) { c.onControlSeen(t); c.tick(t); }
        CHECK(c.follower.voiceCall(0) == 0, "the hang time ends it");
    }
    // ---- the follower's actions around voice radios
    {
        Controller c;
        setupSite(c);
        c.setVoiceRadios(1);
        c.tgs.upsert(7001).priority = 5;
        c.tgs.upsert(7002).priority = 1;
        feed(c, gen::grpVGrant(4, 0x5010, 7001, 1), 1.0);
        c.takeActions();
        const uint64_t first = c.follower.voiceCall(0);
        feed(c, gen::grpVGrant(4, 0x5011, 7002, 1), 1.1);   // preempts radio 0
        auto a = c.takeActions();
        const int stop = indexOf(a, Action::RecordStop, 0), rel = indexOf(a, Action::ReleaseRadio, 0), ret = indexOf(a, Action::Retune, 0), start = indexOf(a, Action::RecordStart, 0);
        CHECK(stop >= 0 && rel > stop && ret > rel && start > ret, "preemption: RecordStop, ReleaseRadio, then Retune and RecordStart on radio 0 (%d %d %d %d)", stop, rel, ret, start);
        CHECK(a[(size_t)stop].callId == first && c.follower.voiceCall(0) != first, "the old call's recording stops");
        // the call moves to another channel: the old one ends, the new one takes the radio
        const uint64_t before = c.follower.voiceCall(0);
        feed(c, gen::grpVGrant(4, 0x5012, 7002, 1), 1.2);
        a = c.takeActions();
        CHECK(indexOf(a, Action::ReleaseRadio, 0) >= 0 && indexOf(a, Action::Retune, 0) > indexOf(a, Action::ReleaseRadio, 0), "moved: released then retuned");
        CHECK(c.log.byId(before) && !c.log.byId(before)->active && c.log.byId(before)->note == "moved", "moved: the old call ended");
        CHECK(c.follower.voiceCall(0) != 0 && c.follower.voiceCall(0) != before, "moved: the new call is on the radio");
        // following switched off: every voice call ends, the radio stays linked
        const uint64_t on = c.follower.voiceCall(0);
        c.endVoiceCalls("following switched off");
        a = c.takeActions();
        CHECK(c.follower.voiceCall(0) == 0 && c.follower.voiceRadios() == 1 && c.follower.audibleCall() == 0, "voice calls end, radio kept");
        CHECK(indexOf(a, Action::RecordStop, 0) >= 0 && indexOf(a, Action::ReleaseRadio, 0) >= 0, "their recording stops, the radio is released");
        CHECK(c.log.byId(on) && !c.log.byId(on)->active, "the call is no longer active");
        CHECK(voiceRadioMuted(false, 0) && !voiceRadioMuted(false, on) && voiceRadioMuted(true, on), "a voice radio with no call is silent");
        // shrinking while a call runs: it ends at once with its recording
        feed(c, gen::grpVGrant(4, 0x5013, 7001, 1), 2.0);
        c.takeActions();
        const uint64_t run = c.follower.voiceCall(0);
        c.setVoiceRadios(0);
        a = c.takeActions();
        CHECK(run && c.log.byId(run) && !c.log.byId(run)->active, "shrinking ends the call at once");
        bool st = false;
        for (const auto& x : a) st |= x.kind == Action::RecordStop && x.callId == run;
        CHECK(st, "and stops its recording");
        // reset with calls running: the recordings stop, the radios stay linked, nothing else is left
        c.setVoiceRadios(2);
        feed(c, gen::grpVGrant(4, 0x5014, 7001, 1), 3.0);
        feed(c, gen::grpVGrant(4, 0x5015, 7002, 1), 3.0);
        c.takeActions();
        const uint64_t r0 = c.follower.voiceCall(0), r1 = c.follower.voiceCall(1);
        c.follower.reset();
        a = c.takeActions();
        int stops = 0;
        for (const auto& x : a) stops += x.kind == Action::RecordStop && (x.callId == r0 || x.callId == r1);
        CHECK(r0 && r1 && stops == 2 && (int)a.size() == 2, "reset: two RecordStops and nothing else (%zu actions)", a.size());
        CHECK(c.follower.voiceRadios() == 2 && c.follower.voiceCall(0) == 0 && c.follower.voiceCall(1) == 0, "reset: radios kept, free");
    }
    // ---- inside the capture with every slice busy: a free voice radio takes the call
    {
        Controller c;
        feed(c, gen::idenUp(1, 851006250, 12500, -45000000, 12500), 0);
        feed(c, gen::netSts(0xBEE00, 0x3A1, 0x1001, 0x70), 0);
        c.setCapture(851.1e6, 2.4e6);
        FollowConfig fc; fc.maxSlices = 1;
        c.follower.setConfig(fc);
        c.setVoiceRadios(1);
        feed(c, gen::grpVGrant(4, 0x1010, 8001, 1), 1.0);
        auto a = c.takeActions();
        CHECK(indexOf(a, Action::OpenSlice, -1) >= 0, "first call in a slice");
        feed(c, gen::grpVGrant(4, 0x1011, 8002, 1), 1.1);
        a = c.takeActions();
        CHECK(indexOf(a, Action::Retune, 0) >= 0 && c.log.records().back().followed, "slices busy: the voice radio takes it");
    }
    // ---- the app's rules for linked receivers
    {
        RxState ctl; ctl.rxId = 1; ctl.family = 50; ctl.running = true; ctl.claimed = true;
        RxState v; v.rxId = 2; v.family = 50; v.voiceFor = 1; v.running = true; v.claimed = true;
        RxState other; other.rxId = 3; other.family = 50; other.running = true; other.claimed = true;
        // who feeds the follower
        CHECK(controlMayFeed(ctl, nullptr) && controlMayFeed(ctl, &ctl), "the control receiver feeds");
        CHECK(!controlMayFeed(v, &ctl) && !controlMayFeed(v, nullptr), "a voice radio never feeds control messages");
        CHECK(!controlMayFeed(other, &ctl), "another receiver does not while the owner runs");
        RxState stopped = other; stopped.running = false;
        CHECK(!controlMayFeed(stopped, nullptr), "a stopped receiver (frozen telemetry) never feeds");
        RxState ctlStopped = ctl; ctlStopped.running = false;
        CHECK(controlMayFeed(other, &ctlStopped), "when the owner stops another may take over");
        RxState adsb = other; adsb.family = 11;
        CHECK(!controlMayFeed(adsb, nullptr), "not a trunked mode");
        // voice telemetry
        CHECK(voiceMayFeed(v, ctl), "a linked running voice radio feeds");
        RxState vs = v; vs.running = false;
        CHECK(!voiceMayFeed(vs, ctl), "a stopped voice radio does not");
        RxState vu = v; vu.claimed = false;
        CHECK(!voiceMayFeed(vu, ctl), "nor one without its radio");
        RxState vl = v; vl.radioLost = true;
        CHECK(!voiceMayFeed(vl, ctl), "nor one whose radio is lost");
        RxState vm = v; vm.family = 9;
        CHECK(!voiceMayFeed(vm, ctl), "nor one in another mode");
        // fates
        CHECK(voiceFate(&v, &ctl, 50, 0, true) == VoiceFate::Keep, "keep");
        CHECK(voiceFate(nullptr, &ctl, 50, 0, true) == VoiceFate::Gone, "voice tab closed");
        CHECK(voiceFate(&vm, &ctl, 50, 0, true) == VoiceFate::UnlinkKeep, "voice tab switched mode");
        CHECK(voiceFate(&vs, &ctl, 50, 0, true) == VoiceFate::UnlinkClose, "voice tab stopped");
        CHECK(voiceFate(&vs, &ctl, 50, 0, false) == VoiceFate::Keep, "still starting");
        CHECK(voiceFate(&vl, &ctl, 50, 0, false) == VoiceFate::UnlinkClose, "unplugged");
        CHECK(voiceFate(&vu, &ctl, 50, 0, true) == VoiceFate::UnlinkClose, "its radio taken");
        RxState ctlDmr = ctl; ctlDmr.family = 9;
        CHECK(voiceFate(&vm, &ctlDmr, 50, 0, true) == VoiceFate::AllClose, "control switched P25 -> DMR: all closed (even if the voice tab followed)");
        CHECK(voiceFate(&v, nullptr, 50, 0, true) == VoiceFate::AllClose, "control closed");
        CHECK(voiceFate(&v, &ctlStopped, 50, 0, true) == VoiceFate::Keep, "control stopped: the radios stay linked (idle)");
        // a takeover starts from the new owner's latest message; the first owner reads what it holds
        CHECK(skipBacklogOnTakeover(1, 3) && !skipBacklogOnTakeover(0, 3) && !skipBacklogOnTakeover(3, 3), "takeover backlog");
        // who may share a mode
        CHECK(linkedPairMayShareMode(ctl, v, &ctl) && linkedPairMayShareMode(v, ctl, &ctl), "control and its voice radio");
        RxState v2 = v; v2.rxId = 4;
        CHECK(linkedPairMayShareMode(v, v2, &ctl), "two voice radios of one control");
        CHECK(!linkedPairMayShareMode(ctl, other, &ctl) && !linkedPairMayShareMode(other, ctl, nullptr), "two unlinked receivers");
        CHECK(!linkedPairMayShareMode(v, other, &ctl), "a voice radio and an unrelated tab");
        RxState va = v; va.family = 11; RxState adsbTab = adsb; adsbTab.running = true;
        CHECK(!linkedPairMayShareMode(va, adsbTab, &ctl), "a voice tab switched to ADS-B shares nothing with an ADS-B tab");
        RxState ctlOtherMode = ctl; ctlOtherMode.family = 11;
        CHECK(!linkedPairMayShareMode(ctlOtherMode, va, &ctlOtherMode), "the exception is for trunked modes only");
    }
    return testSummary("p25 trunk voice");
}
