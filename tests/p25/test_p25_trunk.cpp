// Trunking logic: the system model from TSBKs, the follow state machine (grant -> follow -> end -> return), priority and lockout, the rotating
// control channel, the talkgroup CSV, encrypted grants, and malformed / random inputs. No RF: the messages go through tsbkBits / tsbkParse first.
#include "dect2/p25/trunk.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::trunk;
#include "../common/test_util.h"

static p25::Tsbk wire(const p25::Tsbk& t) {   // through the bit layout and the CRC like a received one
    uint8_t b[96];
    p25::tsbkBits(t, b);
    p25::Tsbk r;
    if (!p25::tsbkParse(b, r)) { printf("FAIL: tsbk did not survive its CRC\n"); fails++; }
    return r;
}

static void feed(Controller& c, const p25::Tsbk& t, double now, int nac = 0x293) { c.onTsbk(wire(t), nac, now); }

static int count(const std::vector<Action>& a, Action::Kind k) { int n = 0; for (const auto& x : a) n += x.kind == k; return n; }

// a site with identifier 1 at 851.00625 MHz / 12.5 kHz: channel 0x1010 = 851.20625 MHz
static void setupSite(Controller& c, double now) {
    feed(c, gen::idenUp(1, 851006250, 12500, -45000000, 12500), now);
    feed(c, gen::netSts(0xBEE00, 0x3A1, 0x1001, 0x70), now);
    feed(c, gen::rfssSts(0x3A1, 1, 5, 0x1001, 0x70), now);
}

static double chHz(int ch) { return 851006250 + (ch & 0xFFF) * 12500.0; }

int main() {
    // ---- system model
    {
        Controller c;
        setupSite(c, 1);
        feed(c, gen::adjSts(0x3A1, 1, 6, 0x1009, 0x70), 1);
        feed(c, gen::sccb(1, 5, 0x1020, 0x1021), 1);
        const SiteInfo& s = c.sys.site();
        CHECK(s.valid && s.wacn == 0xBEE00 && s.sysId == 0x3A1 && s.rfss == 1 && s.site == 5 && s.nac == 0x293, "site %05X %03X %d %d", s.wacn, s.sysId, s.rfss, s.site);
        CHECK(std::fabs(s.ccHz - chHz(0x1001)) < 1, "cc %f", s.ccHz);
        CHECK(c.sys.adjacent().size() == 1 && std::fabs(c.sys.adjacent()[0].ccHz - chHz(0x1009)) < 1, "adjacent");
        const auto cand = c.sys.controlCandidates();
        CHECK(cand.size() == 4 && std::fabs(cand[0] - chHz(0x1001)) < 1, "%zu candidates", cand.size());
        CHECK(c.sys.iden(1).valid && std::fabs(c.sys.iden(1).spacingHz - 12500) < 1 && std::fabs(c.sys.iden(1).txOffsetHz + 45e6) < 1, "iden");
        // VU identifier: 6.25 kHz, +5 MHz
        feed(c, gen::idenUpVu(2, 460000000, 6250, 5000000, 4), 1);
        CHECK(c.sys.iden(2).vu && std::fabs(c.sys.iden(2).spacingHz - 6250) < 1 && std::fabs(c.sys.iden(2).txOffsetHz - 5e6) < 1 && std::fabs(c.sys.iden(2).bwHz - 6250) < 1, "vu iden");
        CHECK(std::fabs(c.sys.channelFreq(0x2004) - 460025000) < 1, "vu channel %f", c.sys.channelFreq(0x2004));
        CHECK(c.sys.channelFreq(0x5001) == 0, "unknown identifier gives 0");
        // TDMA identifier, two slots: channel 5 is carrier 2 slot 1
        feed(c, gen::idenUpTdma(3, 3, 770000000, 12500, -30000000), 1);
        int slot = -2; bool tdma = false;
        const double f = c.sys.channelFreq(0x3005, &slot, &tdma);
        CHECK(tdma && slot == 1 && std::fabs(f - (770000000 + 2 * 12500.0)) < 1, "tdma %f slot %d", f, slot);
        feed(c, gen::idenUpTdma(4, 9, 770000000, 12500, 0), 1);   // OP25 tk_p25.py: channel types 5..15 have two slots
        CHECK(std::fabs(c.sys.channelFreq(0x4001, &slot, &tdma) - 770000000) < 1 && slot == 1 && tdma, "channel type 9 is two-slot");
        feed(c, gen::idenUpTdma(5, 4, 770000000, 12500, 0), 1);   // type 4: four slots
        CHECK(c.sys.channelFreq(0x5001) == 0, "a four-slot channel type is not placed");
    }
    // ---- Phase 1 against Phase 2 calls; a Phase 2 grant waits for the system identity (the burst scrambling seed)
    {
        Controller c;
        feed(c, gen::idenUp(1, 851006250, 12500, -45000000, 12500), 0);
        feed(c, gen::idenUpTdma(3, 3, 770000000, 12500, -30000000), 0);
        c.setVoiceRadios(1);
        feed(c, gen::grpVGrant(4, 0x1010, 1201, 555), 1.0);   // FDMA: Phase 1, IMBE
        CHECK(!c.log.records().empty() && !c.log.records().back().p25p2 && c.log.records().back().slot == -1, "a Phase 1 grant is not marked Phase 2 (IMBE)");
        const size_t n0 = c.log.records().size();
        feed(c, gen::grpVGrant(4, 0x3005, 1301, 556), 1.1);   // TDMA, but WACN / system / NAC not heard yet
        CHECK(c.log.records().size() == n0 && c.phase2Waiting() == 1 && !c.phase2Ready(), "a Phase 2 grant before NET_STS waits (%zu records)", c.log.records().size());
        bool said = false;
        for (const auto& m : c.messages) said |= m.find("waits for the system ID") != std::string::npos;
        CHECK(said, "the wait is said in the messages");
        const uint64_t g0 = c.phase2SeedGen();
        feed(c, gen::netSts(0xBEE00, 0x3A1, 0x1001, 0x70), 1.5);
        CHECK(c.phase2Ready() && c.phase2SeedGen() == g0 + 1, "the identity is known");
        CHECK(c.log.records().size() == n0 + 1 && c.log.records().back().p25p2 && c.log.records().back().slot == 1 && c.log.records().back().tg == 1301,
              "the held Phase 2 grant is followed once the identity is known");
        feed(c, gen::netSts(0xBEE00, 0x3A1, 0x1001, 0x70), 2.0);
        CHECK(c.phase2SeedGen() == g0 + 1, "the same identity does not re-seed");
        feed(c, gen::netSts(0xBEE01, 0x3A1, 0x1001, 0x70), 2.5);
        CHECK(c.phase2SeedGen() == g0 + 2, "a changed WACN re-seeds the running Phase 2 radios");
        Controller d;   // an old held grant is not followed, but logged once as missed
        feed(d, gen::idenUpTdma(3, 3, 770000000, 12500, -30000000), 0);
        feed(d, gen::grpVGrant(4, 0x3005, 1301, 556), 1.0);
        feed(d, gen::netSts(0xBEE00, 0x3A1, 0x1001, 0x70), 9.0);
        CHECK(d.log.records().size() == 1 && d.log.records()[0].missed && !d.log.records()[0].followed && d.log.records()[0].tg == 1301 &&
              d.log.records()[0].note == "missed (waiting for the system ID)", "a Phase 2 grant held for more than 3 s is logged as missed (%zu rows)", d.log.records().size());
    }
    // ---- an encrypted call found out by the voice radio: logged once, the radio released, its grant updates do not follow it again
    {
        Controller c;
        setupSite(c, 0);
        FollowConfig fc; fc.hangSec = 2;
        c.follower.setConfig(fc);
        c.setVoiceRadios(1);
        feed(c, gen::grpVGrant(0, 0x1010, 4401, 77), 1.0);   // no E bit in the grant
        auto a = c.takeActions();
        CHECK(count(a, Action::Retune) == 1 && c.log.records().size() == 1, "the call is followed");
        const uint64_t id = c.log.records().back().id;
        c.onEncrypted(id, 1.5);   // the voice radio hears it is encrypted
        c.tick(1.6);
        a = c.takeActions();
        CHECK(count(a, Action::ReleaseRadio) == 1 && c.log.byId(id)->encrypted && !c.log.byId(id)->active, "released and marked encrypted");
        int retunes = 0;
        for (int k = 0; k < 4; k++) {   // the updates keep coming while the call lasts
            feed(c, gen::grpVGrantUpd(0x1010, 4401, 0x1010, 4401), 2.0 + k);
            c.tick(2.1 + k);
            retunes += count(c.takeActions(), Action::Retune);
        }
        CHECK(retunes == 0 && c.log.records().size() == 1, "updates of the encrypted call: %d retunes, %zu rows (want 0, 1)", retunes, c.log.records().size());
        feed(c, gen::grpVGrant(0, 0x1010, 4401, 78), 12.0);   // after a gap: a new call of that talkgroup is followed
        CHECK(c.log.records().size() == 2 && count(c.takeActions(), Action::Retune) == 1, "a later new call is followed");
        feed(c, gen::grpVGrant(0, 0x1011, 4402, 79), 12.1);   // another talkgroup is not affected
        CHECK(c.log.records().size() == 3, "another talkgroup is logged");
    }
    // ---- grants, calls, the follow cycle inside a wide capture
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);   // covers the whole 851 MHz block
        FollowConfig fc; fc.maxSlices = 2; fc.hangSec = 2;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(0x04, 0x1010, 1201, 555), 1.0);
        auto a = c.takeActions();
        CHECK(count(a, Action::OpenSlice) == 1 && count(a, Action::RecordStart) == 1, "open + record: %zu actions", a.size());
        CHECK(a.size() >= 1 && std::fabs(a[0].freqHz - chHz(0x1010)) < 1, "slice frequency");
        CHECK(c.follower.state() == FollowState::Following, "state %s", followStateName(c.follower.state()));
        CHECK(c.nowTalking().size() == 1 && c.nowTalking()[0]->tg == 1201 && c.nowTalking()[0]->src == 555, "now talking");
        feed(c, gen::grpVGrantUpd(0x1010, 1201, 0x1010, 1201), 2.0);   // a repeat keeps it going
        feed(c, gen::grpVGrant(0x04, 0x1010, 1201, 555), 2.5);
        CHECK(c.takeActions().empty(), "repeats do not open another slice");
        c.tick(3.0);
        CHECK(c.nowTalking().size() == 1, "still going 0.5 s after the last repeat");
        c.tick(5.0);
        a = c.takeActions();
        CHECK(count(a, Action::CloseSlice) == 1 && count(a, Action::RecordStop) == 1, "close + stop record");
        CHECK(c.nowTalking().empty() && c.follower.state() == FollowState::ControlHold, "ended, back to hold (%s)", followStateName(c.follower.state()));
        CHECK(c.log.records().size() == 1 && std::fabs(c.log.records()[0].durSec - 1.5) < 1e-6 && c.log.records()[0].followed && std::fabs(c.log.records()[0].freqHz - chHz(0x1010)) < 1, "log: dur %.2f", c.log.records()[0].durSec);
        // a terminator ends a call at the next tick
        feed(c, gen::grpVGrant(0x04, 0x1011, 1202, 556), 10.0);
        c.takeActions();
        const uint64_t id = c.log.records().back().id;
        c.onVoice(id, 10.5);
        c.onTerminator(id, 10.6);
        c.tick(10.7);
        CHECK(count(c.takeActions(), Action::CloseSlice) == 1 && !c.log.records().back().active, "terminator ends the call");
        // unit to unit
        feed(c, gen::uuVGrant(0x1012, 777, 888), 20.0);
        const CallRecord& u = c.log.records().back();
        CHECK(!u.group && u.tg == 777 && u.src == 888 && u.followed, "unit to unit call");
        c.tick(30.0);
    }
    // ---- slices, priority, lockout, scan lists
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        FollowConfig fc; fc.maxSlices = 2;
        c.follower.setConfig(fc);
        c.tgs.upsert(2001).priority = 5;
        c.tgs.upsert(2002).priority = 3;
        c.tgs.upsert(2003).priority = 1;
        c.tgs.upsert(2004).lockout = true;
        feed(c, gen::grpVGrant(4, 0x1010, 2001, 1), 1);
        feed(c, gen::grpVGrant(4, 0x1011, 2002, 2), 1.1);
        c.takeActions();
        CHECK(c.follower.activeSlices() == 2, "two slices");
        feed(c, gen::grpVGrant(4, 0x1012, 2009, 3), 1.2);   // no priority: busy, not followed
        CHECK(c.takeActions().empty() && c.log.records().back().missed && c.follower.missedCalls() == 1, "third call is busy");
        feed(c, gen::grpVGrant(4, 0x1013, 2003, 4), 1.3);   // highest priority preempts the lowest (2001)
        auto a = c.takeActions();
        CHECK(count(a, Action::CloseSlice) == 1 && count(a, Action::OpenSlice) == 1, "preempt: %zu actions", a.size());
        CHECK(c.follower.audibleCall() == c.log.records().back().id, "the priority call is the audible one");
        bool lowGone = false;
        for (const auto& r : c.log.records()) if (r.tg == 2001 && !r.active) lowGone = true;
        CHECK(lowGone, "the preempted call ended");
        feed(c, gen::grpVGrant(4, 0x1014, 2004, 5), 1.4);
        CHECK(c.takeActions().empty() && c.log.records().back().note == "locked out", "lockout: '%s'", c.log.records().back().note.c_str());
        // scan-only: only listed talkgroups with listen on
        Controller d;
        setupSite(d, 0);
        d.setCapture(851.1e6, 2.4e6);
        d.tgs.upsert(10).list = "Fire";
        d.tgs.upsert(11).list = "Police";
        d.tgs.setScanOnly(true);
        d.tgs.enableList("Fire", true);
        feed(d, gen::grpVGrant(4, 0x1010, 10, 1), 1);
        feed(d, gen::grpVGrant(4, 0x1011, 11, 1), 1);
        feed(d, gen::grpVGrant(4, 0x1012, 12, 1), 1);
        CHECK(d.follower.activeSlices() == 1 && d.log.records().size() == 3, "scan list: %d slices", d.follower.activeSlices());
        CHECK(d.log.records()[1].note == "its list is off" && d.log.records()[2].note == "not in the scan list", "notes '%s' '%s'", d.log.records()[1].note.c_str(), d.log.records()[2].note.c_str());
    }
    // ---- encrypted: logged, never followed, never recorded
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        feed(c, gen::grpVGrant(0x40, 0x1010, 3001, 9), 1);   // service options bit 0x40
        c.tgs.upsert(3002).mode = "DE";
        feed(c, gen::grpVGrant(0, 0x1011, 3002, 9), 1);       // the list says encrypted
        CHECK(c.takeActions().empty() && c.follower.activeSlices() == 0, "no actions for encrypted calls");
        const auto& r = c.log.records();
        CHECK(r.size() == 2 && r[0].encrypted && r[1].encrypted && !r[0].followed && !r[1].followed, "logged as encrypted");
        CHECK(c.log.exportCsv().find("encrypted") != std::string::npos, "csv says encrypted");
    }
    // ---- outside the capture: hop with one radio, return; or say it needs more
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(chHz(0x1001), 0);   // only the control channel
        feed(c, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
        FollowConfig fc; fc.hangSec = 1.5;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(4, 0x5010, 4001, 1), 1.0);
        auto a = c.takeActions();
        CHECK(count(a, Action::Retune) == 1 && a[0].kind == Action::Retune && std::fabs(a[0].freqHz - (856000000 + 16 * 12500.0)) < 1, "retune to the voice channel");
        CHECK(c.follower.state() == FollowState::Hopped, "hopped");
        c.onVoice(c.log.records().back().id, 2.0);
        c.tick(3.0);
        CHECK(c.follower.state() == FollowState::Hopped, "voice keeps it there");
        c.tick(4.0);
        a = c.takeActions();
        CHECK(count(a, Action::Retune) == 1 && std::fabs(a.back().freqHz - chHz(0x1001)) < 1 && c.follower.state() == FollowState::Returning, "back to the control channel (%s)", followStateName(c.follower.state()));
        feed(c, gen::netSts(0xBEE00, 0x3A1, 0x1001, 0x70), 4.5);
        CHECK(c.follower.state() == FollowState::ControlHold, "control channel is back");
        // a radio that may not retune: says what is needed
        Controller d;
        setupSite(d, 0);
        d.setCapture(chHz(0x1001), 0);
        feed(d, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
        FollowConfig f2; f2.allowRetune = false;
        d.follower.setConfig(f2);
        feed(d, gen::grpVGrant(4, 0x5010, 4001, 1), 1.0);
        a = d.takeActions();
        bool said = false;
        for (const auto& x : a) if (x.kind == Action::Notice && x.text.find("needs a wider capture or a second radio") != std::string::npos) said = true;
        CHECK(said && d.log.records().back().missed && d.log.records().back().note.find("second radio") != std::string::npos, "needs a wider capture or a second radio");
        // while one call is followed inside the capture a second one outside does not take the radio away
        Controller e;
        setupSite(e, 0);
        e.setCapture(851.1e6, 1.0e6);
        feed(e, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
        feed(e, gen::grpVGrant(4, 0x1010, 5001, 1), 1);
        e.takeActions();
        feed(e, gen::grpVGrant(4, 0x5010, 5002, 1), 1.1);
        a = e.takeActions();
        CHECK(count(a, Action::Retune) == 0 && e.log.records().back().missed, "no hop away from a running call");
    }
    // ---- voice radios: calls outside the capture go to other radios; the control channel is never left
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(chHz(0x1001), 0);   // the control radio only has the control channel
        feed(c, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
        FollowConfig fc; fc.hangSec = 1.5;
        c.follower.setConfig(fc);
        c.setVoiceRadios(2);
        CHECK(c.follower.voiceRadios() == 2, "two voice radios");
        c.tgs.upsert(6001).priority = 6;
        c.tgs.upsert(6002).priority = 5;
        c.tgs.upsert(6003).priority = 8;   // lower than both running calls
        c.tgs.upsert(6004).priority = 1;   // higher than both
        auto vch = [](int n) { return 856000000 + n * 12500.0; };
        bool leftControl = false;
        auto look = [&](const std::vector<Action>& a) {
            for (const auto& x : a) if (x.kind == Action::Retune && x.radio < 0) leftControl = true;
            const FollowState s = c.follower.state();
            if (s == FollowState::Hopped || s == FollowState::Returning) leftControl = true;
        };
        auto retuneOn = [](const std::vector<Action>& a, int radio, double hz) {
            for (const auto& x : a) if (x.kind == Action::Retune && x.radio == radio && std::fabs(x.freqHz - hz) < 1) return true;
            return false;
        };
        auto released = [](const std::vector<Action>& a, int radio) {
            for (const auto& x : a) if (x.kind == Action::ReleaseRadio && x.radio == radio) return true;
            return false;
        };
        feed(c, gen::grpVGrant(4, 0x5010, 6001, 1), 1.0);
        auto a = c.takeActions(); look(a);
        const uint64_t call1 = c.log.records().back().id;
        CHECK(retuneOn(a, 0, vch(16)) && c.follower.voiceCall(0) == call1 && c.log.records().back().followed, "first call on voice radio 0");
        CHECK(c.follower.state() == FollowState::ControlHold, "control stays held (%s)", followStateName(c.follower.state()));
        bool rec = false;
        for (const auto& x : a) rec |= x.kind == Action::RecordStart && x.radio == 0 && x.callId == call1;
        CHECK(rec, "recording starts on voice radio 0");
        feed(c, gen::grpVGrant(4, 0x5011, 6002, 1), 1.1);
        a = c.takeActions(); look(a);
        const uint64_t call2 = c.log.records().back().id;
        CHECK(retuneOn(a, 1, vch(17)) && c.follower.voiceCall(1) == call2, "second call on voice radio 1");
        CHECK(c.follower.audibleCall() == call2, "the higher priority call is the audible one");
        // a repeat of a running call takes no radio
        feed(c, gen::grpVGrantUpd(0x5010, 6001, 0x5011, 6002), 1.2);
        a = c.takeActions(); look(a);
        CHECK(count(a, Action::Retune) == 0, "grant updates take no radio");
        // both busy, a lower priority call: missed
        feed(c, gen::grpVGrant(4, 0x5012, 6003, 1), 1.3);
        a = c.takeActions(); look(a);
        CHECK(count(a, Action::Retune) == 0 && c.log.records().back().missed && c.log.records().back().note == "missed (all voice radios busy)", "missed: %s", c.log.records().back().note.c_str());
        CHECK(c.follower.missedCalls() == 1, "one missed");
        // both busy, a higher priority call: preempts the lowest priority one (6001 on radio 0)
        feed(c, gen::grpVGrant(4, 0x5013, 6004, 1), 1.4);
        a = c.takeActions(); look(a);
        const uint64_t call4 = c.log.records().back().id;
        CHECK(retuneOn(a, 0, vch(19)) && c.follower.voiceCall(0) == call4 && c.follower.voiceCall(1) == call2, "preempts radio 0");
        const CallRecord* r1 = c.log.byId(call1);
        CHECK(r1 && !r1->active && r1->note.find("preempted") != std::string::npos, "the preempted call ended");
        CHECK(c.follower.audibleCall() == call4, "the new call is audible");
        // voice keeps them; the terminator ends one; silence the other
        c.onVoice(call4, 2.5); c.onVoice(call2, 2.5);
        c.tick(3.0);
        a = c.takeActions(); look(a);
        CHECK(c.follower.voiceCall(0) == call4 && c.follower.voiceCall(1) == call2, "voice keeps both");
        c.onTerminator(call2, 3.1);
        c.tick(3.2);
        a = c.takeActions(); look(a);
        CHECK(released(a, 1) && c.follower.voiceCall(1) == 0 && c.follower.voiceCall(0) == call4, "terminator frees voice radio 1");
        c.tick(4.5);
        a = c.takeActions(); look(a);
        CHECK(released(a, 0) && c.follower.voiceCall(0) == 0, "hang time frees voice radio 0");
        CHECK(!leftControl, "the control channel was never left");
        CHECK(c.follower.state() == FollowState::ControlHold, "still holding");
        // a voice radio goes away during a call (unplugged, the tab closed): the call ends, the others move down
        feed(c, gen::grpVGrant(4, 0x5014, 6002, 1), 5.0);
        feed(c, gen::grpVGrant(4, 0x5015, 6004, 1), 5.0);
        a = c.takeActions(); look(a);
        const uint64_t onR1 = c.follower.voiceCall(1);
        const uint64_t onR0 = c.follower.voiceCall(0);
        CHECK(onR0 && onR1, "two calls running");
        c.dropVoiceRadio(0, "voice radio removed");
        CHECK(c.follower.voiceRadios() == 1 && c.follower.voiceCall(0) == onR1, "radio 1 is now radio 0");
        c.tick(5.1);
        a = c.takeActions(); look(a);
        const CallRecord* rd = c.log.byId(onR0);
        CHECK(rd && !rd->active && rd->note == "voice radio removed", "the call on the removed radio ended");
        CHECK(c.log.byId(onR1) && c.log.byId(onR1)->active, "the other call goes on");
        bool stop = false;
        for (const auto& x : a) stop |= x.kind == Action::RecordStop && x.callId == onR0;
        CHECK(stop, "its recording stops");
        // no voice radios: exactly the old behaviour (hop the control radio)
        c.setVoiceRadios(0);
        c.tick(5.2);
        a = c.takeActions(); look(a);
        CHECK(c.log.byId(onR1) && !c.log.byId(onR1)->active, "calls on removed radios end");
        CHECK(!leftControl, "the control channel was never left while voice radios were linked");
        feed(c, gen::grpVGrant(4, 0x5016, 6001, 1), 6.0);
        a = c.takeActions();
        CHECK(retuneOn(a, -1, vch(22)) && c.follower.state() == FollowState::Hopped, "back to hopping (%s)", followStateName(c.follower.state()));
    }
    // ---- voice radios on NXDN (no control channel named), and slices inside the capture still come first
    {
        Controller c;
        c.nxdn.setChannel(513, 451.0125e6);
        c.setCapture(451.5e6, 0);
        c.setVoiceRadios(1);
        c.onControlSeen(0.5);
        c.onNxdnAssignment(1, 4321, 77, 513, 0, 1.0);
        bool r0 = false, own = false;
        for (const Action& x : c.takeActions()) if (x.kind == Action::Retune) { r0 |= x.radio == 0 && std::fabs(x.freqHz - 451.0125e6) < 1; own |= x.radio < 0; }
        CHECK(r0 && !own && c.follower.state() == FollowState::ControlHold, "nxdn call on the voice radio");
        for (double t = 1.5; t < 6; t += 0.5) { c.onControlSeen(t); c.tick(t); }
        own = false; bool rel = false;
        for (const Action& x : c.takeActions()) { own |= x.kind == Action::Retune && x.radio < 0; rel |= x.kind == Action::ReleaseRadio && x.radio == 0; }
        CHECK(rel && !own && !c.log.records()[0].active, "ends without a trip home");
        Controller s;
        setupSite(s, 0);
        s.setCapture(851.1e6, 2.4e6);
        s.setVoiceRadios(1);
        feed(s, gen::grpVGrant(4, 0x1010, 3001, 9), 1);
        auto a = s.takeActions();
        CHECK(count(a, Action::OpenSlice) == 1 && count(a, Action::Retune) == 0, "inside the capture a slice is still used");
    }
    // ---- rotating control channel
    {
        Controller c;
        setupSite(c, 0);
        feed(c, gen::sccb(1, 5, 0x1020, 0x1021), 0);
        c.setCapture(chHz(0x1001), 0);
        FollowConfig fc; fc.ccLossSec = 2; fc.ccDwellSec = 1;
        c.follower.setConfig(fc);
        c.tick(1.0);
        CHECK(c.follower.state() == FollowState::ControlHold, "holding");
        c.tick(3.0);
        auto a = c.takeActions();
        CHECK(c.follower.state() == FollowState::Searching && count(a, Action::Retune) == 1, "control lost: searching");
        c.tick(4.5);
        a = c.takeActions();
        CHECK(count(a, Action::Retune) == 1 && std::fabs(a[0].freqHz - chHz(0x1020)) < 1, "next candidate");
        feed(c, gen::rfssSts(0x3A1, 1, 5, 0x1020, 0x70), 5.0);   // heard it
        CHECK(c.follower.state() == FollowState::ControlHold, "found, holding");
    }
    // ---- talkgroup CSV
    {
        TalkgroupList l;
        const std::string csv = "Decimal,Hex,Alpha Tag,Mode,Description,Tag,Category\r\n"
                                "1201,4B1,\"Fire Disp\",D,\"Fire, dispatch\",Fire Dispatch,Metro Fire\r\n"
                                "1202,4B2,Fire Tac 1,DE,\"Quote \"\"x\"\" here\",Fire Tac,Metro Fire\r\n"
                                "abc,,bad row,D,,,\r\n"
                                "16777216,,out of range,D,,,\r\n"
                                "\r\n"
                                "1203,,PD,T,,Law Dispatch,Metro PD\r\n";
        std::string err; int skipped = 0;
        const int n = l.importCsv(csv, &err, &skipped);
        CHECK(n == 3 && skipped == 2, "%d rows, %d skipped", n, skipped);
        CHECK(l.find(1201) && l.find(1201)->alpha == "Fire Disp" && l.find(1201)->desc == "Fire, dispatch" && l.find(1201)->category == "Metro Fire", "fields");
        CHECK(l.find(1202) && l.find(1202)->desc == "Quote \"x\" here" && l.find(1202)->encryptedMode(), "quotes and mode");
        l.find(1201)->priority = 2; l.find(1203)->lockout = true; l.find(1203)->list = "PD, all";
        const std::string out = l.exportCsv();
        TalkgroupList m;
        int sk2 = 0;
        CHECK(m.importCsv(out, nullptr, &sk2) == 3 && sk2 == 0, "round trip");
        CHECK(m.find(1201)->priority == 2 && m.find(1203)->lockout && m.find(1203)->list == "PD, all" && m.find(1202)->desc == "Quote \"x\" here", "round trip fields");
        // columns in another order and hex only
        TalkgroupList h;
        CHECK(h.importCsv("Hex,Alpha Tag,Decimal\n4B1,A,\n,B,1300\n") == 2 && h.find(1201) && h.find(1300), "reordered / hex-only columns");
        // no header: RadioReference order
        TalkgroupList n2;
        CHECK(n2.importCsv("500,1F4,Name,D,Desc,Tag,Cat\n") == 1 && n2.find(500)->alpha == "Name", "headerless");
        CHECK(n2.importCsv("") == 0 && n2.importCsv("\n\n") == 0, "empty input");
    }
    // ---- call log CSV
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        c.tgs.upsert(1201).alpha = "Fire, Disp";
        feed(c, gen::grpVGrant(4, 0x1010, 1201, 555), 1);
        c.tick(10);
        const std::string csv = c.log.exportCsv(&c.tgs);
        CHECK(csv.find("\"Fire, Disp\"") != std::string::npos && csv.find("851.20625") != std::string::npos && csv.find(",555,") != std::string::npos, "log csv: %s", csv.c_str());
    }
    // ---- DMR Tier III grants (TS 102 361-4: PV_GRANT 0x30, TV_GRANT 0x31, BTV_GRANT 0x32; channel 12 bits, timeslot, late entry, emergency, offset)
    {
        Controller c;
        c.dmr.setChannel(3, 450.125e6);
        c.setCapture(450.1e6, 1e6);
        uint8_t d[8] = {0x00, 0x30, 0x00, 0x00, 0x64, 0x00, 0x00, 0x2A};   // channel 3, slot 0, talkgroup 100, source 42
        c.onDmrCsbk(0x31, 0, d, 1.0);
        CHECK(c.log.records().size() == 1 && c.log.records()[0].dmr && c.log.records()[0].group && c.log.records()[0].tg == 100 && c.log.records()[0].src == 42
              && c.log.records()[0].followed && std::fabs(c.log.records()[0].freqHz - 450.125e6) < 1, "dmr talkgroup grant");
        d[1] = 0x38 | 0x02; d[4] = 0x65;   // slot 1, emergency, talkgroup 101
        c.onDmrCsbk(0x31, 0, d, 5.0);
        CHECK(c.log.records().size() == 2 && c.log.records()[1].tg == 101 && c.log.records()[1].slot == 1 && c.log.records()[1].emergency, "dmr slot and emergency");
        d[4] = 0x66;
        c.onDmrCsbk(0x30, 0, d, 8.0);   // a private call
        CHECK(c.log.records().size() == 3 && !c.log.records()[2].group, "dmr private grant");
        c.onDmrCsbk(0x31, 0x10, d, 9.0);   // a manufacturer's
        CHECK(c.log.records().size() == 3, "manufacturer CSBK ignored");
    }
    // ---- NXDN Type C (VCALL_ASSGN: channel number -> the user's channel table; the one radio leaves the control channel for the call)
    {
        Controller c;
        c.nxdn.setChannel(513, 451.0125e6);
        c.setCapture(451.5e6, 0);
        c.onControlSeen(0.5);
        c.onNxdnAssignment(1, 4321, 77, 513, 0x80, 1.0);
        bool retuned = false;
        for (const Action& a : c.takeActions()) retuned |= a.kind == Action::Retune && std::fabs(a.freqHz - 451.0125e6) < 1;
        CHECK(retuned && c.log.records().size() == 1 && c.log.records()[0].tg == 77 && c.log.records()[0].src == 4321 && c.log.records()[0].group
              && c.log.records()[0].emergency && !c.log.records()[0].dmr && c.log.records()[0].followed, "nxdn group grant followed");
        for (double t = 1.5; t < 12; t += 0.5) c.tick(t);   // no voice heard on the channel: the call ends and the radio goes home
        bool home = false;
        for (const Action& a : c.takeActions()) home |= a.kind == Action::Retune && std::fabs(a.freqHz - 451.5e6) < 1;
        CHECK(home && !c.log.records()[0].active, "back on the control channel after the call");
        // a call whose voice is heard on its channel stays followed past the hang time, and ends once the voice stops
        c.onControlSeen(12.5);
        c.onNxdnAssignment(1, 4321, 78, 513, 0, 12.6);
        c.takeActions();
        const uint64_t id = c.follower.audibleCall();
        for (double t = 12.7; t < 20; t += 0.5) { c.onVoice(id, t); c.tick(t); }
        CHECK(id && c.log.byId(id) && c.log.byId(id)->active, "voice keeps the hopped call");
        for (double t = 20; t < 24; t += 0.5) c.tick(t);
        CHECK(c.log.byId(id) && !c.log.byId(id)->active, "the call ends when its voice stops");
        c.takeActions();
        // another system (the scanner moves on): reset, then its first call returns to the new system's frequency, not the old one
        c.follower.reset();
        c.nxdn.setChannel(600, 452.0e6);
        c.setCapture(451.8e6, 0);
        c.onControlSeen(30);
        c.onNxdnAssignment(1, 5, 6, 600, 0, 30.1);
        c.takeActions();
        for (double t = 30.5; t < 36; t += 0.5) c.tick(t);
        bool homeNew = false, homeOld = false;
        for (const Action& a : c.takeActions()) if (a.kind == Action::Retune) { homeNew |= std::fabs(a.freqHz - 451.8e6) < 1; homeOld |= std::fabs(a.freqHz - 451.5e6) < 1; }
        CHECK(homeNew && !homeOld, "after a reset the follower goes home to the new system");
        c.onNxdnAssignment(4, 4321, 9, 700, 0, 40.0);   // a private call on a channel missing from the table
        CHECK(!c.log.records().back().group && !c.log.records().back().followed, "unknown channel: logged, not followed");
        CHECK(c.nxdn.grants() == 4, "nxdn grants counted");
        Grant g;
        CHECK(!c.nxdn.onAssignment(1, 1, 2, 1024, 0, g) && !c.nxdn.onAssignment(1, 1, 2, -1, 0, g), "channel out of range refused");
    }
    // ---- malformed and random input
    {
        srand(7);
        Controller c;
        c.setCapture(851.1e6, 2.4e6);
        c.tgs.setScanOnly(true);
        FollowConfig fc; fc.maxSlices = 3;
        c.follower.setConfig(fc);
        double now = 0;
        FollowState prevState = c.follower.state();
        for (int i = 0; i < 20000; i++) {
            p25::Tsbk t;
            t.opcode = rand() % 64; t.mfid = rand() % 5 ? 0 : rand() % 256;
            t.args = ((uint64_t)rand() << 40) ^ ((uint64_t)rand() << 20) ^ (uint64_t)rand();
            if (i % 7 == 0) t.opcode = (int[]){0x00, 0x02, 0x03, 0x04, 0x33, 0x34, 0x39, 0x3A, 0x3B, 0x3C, 0x3D}[rand() % 11];
            now += (rand() % 100) / 100.0;
            c.onTsbk(t, rand() & 0xFFF, now);
            if (i % 5 == 0) c.onTsbkBad(now);
            if (i % 3 == 0) c.tick(now);
            // invariants with voice radios linked: no hop of the control radio for a call, no new Hopped state, every voice call active
            const bool voiceOn = c.follower.voiceRadios() > 0;
            for (const Action& a : c.takeActions())
                if (voiceOn && a.kind == Action::Retune && a.radio < 0 && a.callId) { CHECK(false, "control radio hopped for a call with voice radios"); break; }
            if (voiceOn && c.follower.state() == FollowState::Hopped && prevState != FollowState::Hopped) CHECK(false, "entered Hopped with voice radios");
            prevState = c.follower.state();
            for (int k = 0; k < c.follower.voiceRadios(); k++)
                if (const uint64_t id = c.follower.voiceCall(k)) if (const CallRecord* r = c.log.byId(id)) if (!r->active) { CHECK(false, "voice radio %d holds an ended call", k); break; }
            if (i % 997 == 0) c.setCapture((rand() % 2) ? 851.1e6 : 0, (rand() % 2) ? 2.4e6 : 0);
            if (i % 701 == 0) c.setVoiceRadios(rand() % 3);
            if (i % 1301 == 0) c.dropVoiceRadio(rand() % 3 - 1, "gone");
            if (c.follower.voiceRadios() > 2) { CHECK(false, "more voice radios than linked"); break; }
            if (i % 500 == 0) { TgEntry& e = c.tgs.upsert(rand() % 5000); e.listen = rand() & 1; e.lockout = rand() % 4 == 0; e.priority = rand() % 11; }
            if (c.follower.activeSlices() > 3) { CHECK(false, "more slices than allowed"); break; }
        }
        std::string junk;
        for (int i = 0; i < 20000; i++) junk += (char)(rand() % 6 == 0 ? '"' : rand() % 6 == 0 ? ',' : rand() % 8 == 0 ? '\n' : rand() % 256);
        TalkgroupList l;
        l.importCsv(junk);
        l.importCsv("Decimal,Hex\n\"unterminated,1\n99999999999999999999,\n-5,\n");
        CHECK(true, "survived");
        uint8_t d[8];
        for (int i = 0; i < 2000; i++) { for (auto& b : d) b = (uint8_t)rand(); c.onDmrCsbk(0x30 + rand() % 4, rand() % 3 ? 0 : 1, d, now += 0.01); }
    }
    return testSummary("p25 trunk");
}
