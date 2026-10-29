// The analog trunked systems in the shared follower (core/src/p25/trunk_analog.cpp), as message logic: the user's channel tables (CSV,
// formula), EDACS messages (group and individual calls, updates, ProVoice, an LCN missing from the table, system ID), LTR words (repetition,
// the turn-off), MPT 1327 GTCs (formula and table, data calls), talkgroup lockout and priority across them, and what a voice radio hears
// of an analog channel (analogVoiceHeard).
#include "dect2/edacs/edacs_proto.h"
#include "dect2/ltr/ltr_proto.h"
#include "dect2/p25/trunk.h"
#include "dect2/p25/trunk_voice.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
using namespace dect2::trunk;
#include "../common/test_util.h"

namespace {
int count(const std::vector<Action>& v, Action::Kind k) { int n = 0; for (const Action& a : v) n += a.kind == k; return n; }
const Action* find(const std::vector<Action>& v, Action::Kind k) { for (const Action& a : v) if (a.kind == k) return &a; return nullptr; }
const CallRecord* last(const Controller& c) { return c.log.records().empty() ? nullptr : &c.log.records().back(); }
// one EDACS frame (standard / networked set unless ea), fed to the controller
void edacsFrame(Controller& c, double t, edacs::Message::Kind k, int lcn, int group, int target = -1, int source = -1, bool digital = false, bool ea = false) {
    edacs::Message m; m.kind = k; m.lcn = lcn; m.group = group; m.target = target; m.source = source; m.digital = digital; m.individual = target >= 0;
    uint32_t a, b;
    edacs::build(m, ea ? edacs::Mode::Extended : edacs::Mode::Standard, a, b);
    c.onEdacs(a, b, t);
}
}

int main() {
    // ---------------------------------------------------------------- channel tables
    {
        ChannelTable t;
        std::string err; int skipped = 0;
        const int n = t.parse("LCN,Frequency\r\n1,851.0125\n2 = 851.5125\n3;852012500\n# a comment\n4\t853.0125  # with a comment\nx,y\n40,851.0\n5,99999\n", 31, &err, &skipped);
        CHECK(n == 4 && skipped == 3, "CSV: %d rows, %d skipped (%s)", n, skipped, err.c_str());
        CHECK(std::fabs(t.freq(1) - 851012500) < 1 && std::fabs(t.freq(2) - 851512500) < 1 && std::fabs(t.freq(3) - 852012500) < 1 && std::fabs(t.freq(4) - 853012500) < 1,
              "frequencies %.0f %.0f %.0f %.0f", t.freq(1), t.freq(2), t.freq(3), t.freq(4));
        CHECK(t.freq(9) == 0, "a channel not in the table is unknown");
        ChannelTable v;
        CHECK(v.parse("1=451.0125 2=451.0375, 3=451.0625\n7,452.5,Fire dispatch\n", 4095) == 4 && std::fabs(v.freq(2) - 451037500) < 1 && std::fabs(v.freq(7) - 452500000) < 1,
              "the older LCN=MHz pairs and extra columns");
        ChannelTable u;
        CHECK(u.parse(t.csv(), 31) == 4 && std::fabs(u.freq(3) - 852012500) < 1, "the table round trips through its CSV");
        u.setFormula(160e6, 12500);
        CHECK(std::fabs(u.freq(100) - (160e6 + 100 * 12500)) < 1 && std::fabs(u.freq(3) - 852012500) < 1, "formula for the rest, the table first");
        CHECK(u.parse("", 31) == 0 && u.size() == 0 && std::fabs(u.freq(3) - (160e6 + 3 * 12500)) < 1, "an empty table keeps the formula");
    }

    FollowConfig fc; fc.record = false; fc.hangSec = 2.0;
    // ---------------------------------------------------------------- EDACS
    {
        Controller c;
        c.follower.setConfig(fc);
        c.edacs.lcns.parse("1,851.0125\n2,851.2625\n3,851.5125\n", 31);
        c.setCapture(851.0125e6, 0);
        c.setVoiceRadios(2);
        using M = edacs::Message;
        c.onEdacs(0xFD03011, 0, 0.1);   // site ID 0x11, control channel LCN 3 (a frame written from dsd-fme's masks)
        CHECK(c.edacs.site() == 0x11, "site ID %d", c.edacs.site());
        edacsFrame(c, 0.2, M::GroupVoice, 3, 0x2A5, -1, 0x1ABC);
        auto a = c.takeActions();
        const Action* r = find(a, Action::Retune);
        CHECK(r && r->radio == 0 && std::fabs(r->freqHz - 851512500) < 1 && !r->p25p2, "group call on LCN 3 -> voice radio 0 on 851.5125 MHz");
        const CallRecord* rec = last(c);
        CHECK(rec && rec->kind == SysKind::Edacs && rec->tg == 0x2A5 && rec->src == 0x1ABC && rec->group && rec->followed && rec->label == "05-045",
              "the call record: EDACS group %d (%s) from LID %d", rec ? rec->tg : -1, rec ? rec->label.c_str() : "", rec ? rec->src : -1);
        { M u; u.kind = M::ChannelUpdate; u.lcn = 3; u.group = 0x2A5; uint32_t x, y; edacs::build(u, edacs::Mode::Standard, x, y); c.onEdacs(x, y, 1.0); }   // the update
        CHECK(count(c.takeActions(), Action::Retune) == 0 && c.log.records().size() == 1, "a channel update refreshes the running call");
        edacsFrame(c, 1.1, M::GroupVoice, 9, 0x100);   // LCN 9 is not in the table
        CHECK(last(c)->note.find("not in your channel list") != std::string::npos && !last(c)->followed, "LCN 9: %s", last(c)->note.c_str());
        edacsFrame(c, 1.15, M::GroupVoice, 30, 0x102);   // LCN 30: "system busy", not a channel
        CHECK(last(c)->tg != 0x102, "an LCN status value is no call");
        edacsFrame(c, 1.2, M::GroupVoice, 2, 0x101, -1, -1, true);   // ProVoice
        CHECK(last(c)->provoice && !last(c)->followed && last(c)->note.find("ProVoice") != std::string::npos, "ProVoice is logged as ProVoice: %s", last(c)->note.c_str());
        edacsFrame(c, 1.3, M::IndivVoice, 1, -1, 12345, 4321);   // an individual call: callee and caller in the frame
        a = c.takeActions();
        rec = last(c);
        CHECK(rec && !rec->group && rec->tg == 12345 && rec->src == 4321 && rec->followed && count(a, Action::Retune) == 1 && find(a, Action::Retune)->radio == 1,
              "individual call to LID %d from %d on voice radio 1", rec ? rec->tg : -1, rec ? rec->src : -1);
        c.tgs.upsert(0x300).lockout = true;
        edacsFrame(c, 1.4, M::GroupVoice, 1, 0x300);
        CHECK(!last(c)->followed && count(c.takeActions(), Action::Retune) == 0, "a locked out group is logged, not followed (%s)", last(c)->note.c_str());
        c.tgs.upsert(0x301).priority = 1;
        edacsFrame(c, 1.5, M::GroupVoice, 2, 0x301);   // both radios busy: the priority group takes one
        a = c.takeActions();
        CHECK(last(c)->followed && count(a, Action::ReleaseRadio) == 1 && count(a, Action::Retune) == 1, "priority 1 takes a busy voice radio");
        const uint64_t onLcn2 = last(c)->id;
        { M d; d.kind = M::UnkeyDrop; d.drop = true; d.lcn = 2; uint32_t x, y; edacs::build(d, edacs::Mode::Standard, x, y); c.onEdacs(x, y, 1.6); }
        c.tick(1.61);
        CHECK(!c.log.byId(onLcn2)->active, "a console drop of LCN 2 ends the call on it");
        // extended addressing: a 16-bit group
        Controller e;
        e.follower.setConfig(fc);
        e.edacs.mode = 1;
        e.edacs.lcns.parse("12,852.5\n", 31);
        e.setCapture(851.0125e6, 0);
        e.setVoiceRadios(1);
        e.onEdacs(0x319BEEF, 0x0312345, 2.0);   // EA analog group call update, LCN 12, group 0xBEEF, emergency, source 0x12345
        rec = last(e);
        CHECK(rec && rec->tg == 0xBEEF && rec->src == 0x12345 && rec->emergency && rec->followed && find(e.takeActions(), Action::Retune), "EA group call followed: group %X", rec ? rec->tg : -1);
        e.onEdacs(0xFC01A2B, 0x0000005, 2.1);
        CHECK(e.edacs.sysId() == 0x1A2B, "EA system ID %X", e.edacs.sysId());
        c.tick(10);
        CHECK(c.nowTalking().empty(), "the calls end with the hang time");
    }
    // ---------------------------------------------------------------- LTR
    {
        Controller c;
        c.follower.setConfig(fc);
        c.ltr.repeaters.parse("1,460.0125\n2,460.0375\n3,460.0625\n5,460.1125\n", 20);
        c.setCapture(460.0125e6, 0);
        c.setVoiceRadios(1);
        c.onLtr(0, 5, 3, 17, 2, 0.0);   // one word (its checksum matched in the receiver) is a call
        auto a = c.takeActions();
        const Action* r = find(a, Action::Retune);
        const CallRecord* rec = last(c);
        CHECK(rec && rec->kind == SysKind::Ltr && rec->tg == ltr::tgNumber(0, 3, 17) && rec->label == "0-03-017" && r && r->radio == 0 && std::fabs(r->freqHz - 460112500) < 1,
              "talkgroup %s on repeater 5", rec ? rec->label.c_str() : "");
        c.onLtr(0, 5, 3, 17, 2, 0.6);
        CHECK(c.log.records().size() == 1 && last(c)->active, "a repeat keeps the call");
        c.onLtr(0, ltr::kTurnOff, 3, 17, 2, 0.8);   // the turn-off
        c.tick(0.81);
        a = c.takeActions();
        CHECK(!last(c)->active && count(a, Action::ReleaseRadio) == 1, "the turn-off word ends the call at once");
        c.onLtr(0, 4, 3, 18, 2, 1.0);   // repeater 4 is not in the table
        CHECK(last(c)->note.find("not in your channel list") != std::string::npos, "repeater 4 missing: %s", last(c)->note.c_str());
        c.onLtr(0, 5, 3, 254, 2, 1.3);   // a callsign ID word is no call
        c.onLtr(0, 5, 3, 255, 2, 1.3);   // idle
        c.onLtr(0, 5, 3, 40, 25, 1.4);   // free 25 is out of range (LTRStandardMessageProcessor): no call
        CHECK(c.log.records().size() == 2, "ids 254 / 255 and a bad free channel are no calls (%zu records)", c.log.records().size());
    }
    // ---------------------------------------------------------------- Passport
    {
        Controller c;
        c.follower.setConfig(fc);
        c.ltr.repeaters.parse("77,451.5000\n", 2047);
        c.setCapture(460.0e6, 0);
        c.setVoiceRadios(1);
        c.onPassport(0, 120, 9, 4321, 0, 81, 0.0);   // LCN 120: not in the table, the band is not known yet
        CHECK(last(c) && last(c)->note.find("not in your channel list") != std::string::npos, "no band yet: %s", last(c) ? last(c)->note.c_str() : "");
        c.onPassport(0, 1792, 9, 6, 1, 81, 0.5);     // idle: site band 6 (460 MHz)
        CHECK(c.ltr.passportBand() == 6, "band %d", c.ltr.passportBand());
        c.onPassport(0, 2047, 9, 4321, 1, 81, 0.6);  // the end of group 4321
        c.tick(0.61); c.takeActions();
        c.onPassport(0, 77, 9, 1234, 0, 81, 1.0);    // in the table
        auto a = c.takeActions();
        const Action* r = find(a, Action::Retune);
        CHECK(last(c) && last(c)->tg == 1234 && last(c)->label == "1234" && r && std::fabs(r->freqHz - 451.5e6) < 1, "group 1234 on LCN 77 from the table");
        c.onPassport(0, 2047, 9, 1234, 1, 81, 1.5);
        c.tick(1.51); a = c.takeActions();
        CHECK(!last(c)->active && count(a, Action::ReleaseRadio) == 1, "the Passport call end word ends the call");
        c.onPassport(0, 100, 9, 555, 1, 81, 2.0);    // type 1, LCN < 1792: a call; from the band: 460 MHz + 100 x 12.5 kHz
        a = c.takeActions();
        r = find(a, Action::Retune);
        CHECK(last(c) && last(c)->tg == 555 && r && std::fabs(r->freqHz - 461.25e6) < 1, "group 555 on LCN 100 from the band plan");
        c.onPassport(0, 100, 9, 556, 1, 2042, 2.5);  // free 2042: a talkgroup assignment, no call
        c.onPassport(0, 100, 9, 557, 6, 81, 2.6);    // radio ID: no call
        CHECK(last(c)->tg == 555, "assignment / radio ID made a call");
    }
    // ---------------------------------------------------------------- MPT 1327
    {
        Controller c;
        c.follower.setConfig(fc);
        c.mpt.channels.setFormula(165.0e6, 12500);
        c.setCapture(165.5e6, 0);   // one radio: it leaves the control channel for the call
        c.onMpt("ALH", 0, 0, -1, -1, false, false, 0.0);
        c.onMpt("GTC", 5, 1234, 2003, 17, false, false, 0.1);
        auto a = c.takeActions();
        const Action* r = find(a, Action::Retune);
        const CallRecord* rec = last(c);
        CHECK(r && r->radio < 0 && std::fabs(r->freqHz - (165.0e6 + 17 * 12500)) < 1 && c.follower.state() == FollowState::Hopped,
              "GTC channel 17 -> the radio hops to %.4f MHz", r ? r->freqHz / 1e6 : 0);
        CHECK(rec && rec->kind == SysKind::Mpt1327 && rec->tg == 5 * 8192 + 1234 && rec->src == 2003 && rec->label == "5/1234", "MPT 1327 call %s from %d", rec ? rec->label.c_str() : "", rec ? rec->src : -1);
        c.onTerminator(rec->id, 0.5);   // the traffic channel's CLEAR
        c.tick(0.51);
        a = c.takeActions();
        CHECK(find(a, Action::Retune) && std::fabs(find(a, Action::Retune)->freqHz - 165.5e6) < 1, "after the CLEAR the radio returns to the control channel");
        c.mpt.channels.parse("17,169.5\n", 1023);
        c.onMpt("ALH", 0, 0, -1, -1, false, false, 0.7);
        c.onMpt("GTC", 5, 1235, 2004, 17, false, false, 1.0);
        r = find(c.takeActions(), Action::Retune);
        CHECK(r && std::fabs(r->freqHz - 169.5e6) < 1, "the table overrides the formula");
        c.onMpt("GTC", 5, 1300, 2005, 18, true, false, 1.1);
        CHECK(last(c)->note.find("data call") != std::string::npos && !last(c)->followed, "a data GTC is logged: %s", last(c)->note.c_str());
        c.onMpt("GTC", 5, 1301, 2006, 18, false, true, 1.2);   // a GTC on a traffic channel is not a new call
        CHECK(last(c)->tg == 5 * 8192 + 1300, "GTCs decoded on a traffic channel are not taken");
    }
    // ---------------------------------------------------------------- what a voice radio hears of an analog channel
    {
        VoiceWatch w;
        uint64_t seq = 1;
        CHECK(analogVoiceHeard(w, 7, false, true, 5, true, seq++) == VoiceHeard::Nothing, "not on the call's channel yet: nothing");
        CHECK(analogVoiceHeard(w, 7, true, false, 6, true, seq++) == VoiceHeard::Nothing, "on the channel, no carrier yet");
        CHECK(analogVoiceHeard(w, 7, true, true, 8, true, seq++) == VoiceHeard::Voice, "the carrier: voice (the data before it does not count)");
        CHECK(analogVoiceHeard(w, 7, true, true, 8, true, seq - 1) == VoiceHeard::Nothing, "an old report is ignored");
        CHECK(analogVoiceHeard(w, 7, true, true, 8, true, seq++) == VoiceHeard::Voice, "still voice");
        CHECK(analogVoiceHeard(w, 7, true, true, 9, false, seq++) == VoiceHeard::Voice, "another call's end signalling does not end it");
        CHECK(analogVoiceHeard(w, 7, true, true, 10, true, seq++) == VoiceHeard::Ended, "its own end signalling ends it");
        CHECK(analogVoiceHeard(w, 8, true, true, 10, true, seq++) == VoiceHeard::Voice && analogVoiceHeard(w, 8, true, false, 10, true, seq++) == VoiceHeard::Nothing,
              "the next call starts fresh: voice, then the carrier drops (the hang time ends it)");
        CHECK(analogVoiceHeard(w, 8, true, true, 2, true, seq++) == VoiceHeard::Voice && analogVoiceHeard(w, 8, true, true, 3, true, seq++) == VoiceHeard::Ended,
              "a receiver reset (the count went down) is followed");
    }
    {
        CHECK(isTrunkFamily(127) && isTrunkFamily(129) && isTrunkFamily(130) && isAnalogTrunkFamily(130) && !isAnalogTrunkFamily(50), "the analog families are trunked");
    }
    return testSummary("p25 trunk analog");
}
