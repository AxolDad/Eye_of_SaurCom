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
uint32_t edacsMsg(edacs::Message::Kind k, int lcn, int group, int lid = 0, bool digital = false, bool update = false) {
    edacs::Message m; m.kind = k; m.lcn = lcn; m.group = group; m.lid = lid; m.digital = digital; m.update = update;
    return edacs::build(m);
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
        c.onEdacs(edacsMsg(edacs::Message::Idle, 0, 0), 0.0);
        { edacs::Message s; s.kind = edacs::Message::SystemId; s.sysId = 0x1A2B; s.site = 4; c.onEdacs(edacs::build(s), 0.1); }
        CHECK(c.edacs.sysId() == 0x1A2B && c.edacs.site() == 4, "system ID %04X site %d", c.edacs.sysId(), c.edacs.site());
        c.onEdacs(edacsMsg(edacs::Message::GroupVoice, 3, 0x2A5), 0.2);
        auto a = c.takeActions();
        const Action* r = find(a, Action::Retune);
        CHECK(r && r->radio == 0 && std::fabs(r->freqHz - 851512500) < 1 && !r->p25p2, "group call on LCN 3 -> voice radio 0 on 851.5125 MHz");
        const CallRecord* rec = last(c);
        CHECK(rec && rec->kind == SysKind::Edacs && rec->tg == 0x2A5 && rec->group && rec->followed && rec->label == "05-045", "the call record: EDACS group %d (%s)", rec ? rec->tg : -1, rec ? rec->label.c_str() : "");
        c.onEdacs(edacsMsg(edacs::Message::GroupVoice, 3, 0x2A5, 0, false, true), 1.0);   // the update: the same call
        CHECK(count(c.takeActions(), Action::Retune) == 0 && c.log.records().size() == 1, "an update refreshes the running call");
        c.onEdacs(edacsMsg(edacs::Message::GroupVoice, 9, 0x100), 1.1);   // LCN 9 is not in the table
        CHECK(last(c)->note.find("not in your channel list") != std::string::npos && !last(c)->followed, "LCN 9: %s", last(c)->note.c_str());
        c.onEdacs(edacsMsg(edacs::Message::GroupVoice, 2, 0x101, 0, true), 1.2);   // ProVoice
        CHECK(last(c)->provoice && !last(c)->followed && last(c)->note.find("ProVoice") != std::string::npos, "ProVoice is logged as ProVoice: %s", last(c)->note.c_str());
        // an individual call: the caller follows in the frame's second message
        c.onEdacs(edacsMsg(edacs::Message::IndivVoice, 1, 0, 12345), 1.3);
        c.onEdacs(edacsMsg(edacs::Message::Caller, 0, 0, 4321), 1.3);
        a = c.takeActions();
        rec = last(c);
        CHECK(rec && !rec->group && rec->tg == 12345 && rec->src == 4321 && rec->followed && count(a, Action::Retune) == 1 && find(a, Action::Retune)->radio == 1,
              "individual call to LID %d from %d on voice radio 1", rec ? rec->tg : -1, rec ? rec->src : -1);
        // lockout and priority work as for P25
        c.tgs.upsert(0x300).lockout = true;
        c.onEdacs(edacsMsg(edacs::Message::GroupVoice, 1, 0x300), 1.4);
        CHECK(!last(c)->followed && count(c.takeActions(), Action::Retune) == 0, "a locked out group is logged, not followed (%s)", last(c)->note.c_str());
        c.tgs.upsert(0x301).priority = 1;
        c.onEdacs(edacsMsg(edacs::Message::GroupVoice, 2, 0x301), 1.5);   // both radios busy: the priority group takes one
        a = c.takeActions();
        CHECK(last(c)->followed && count(a, Action::ReleaseRadio) == 1 && count(a, Action::Retune) == 1, "priority 1 takes a busy voice radio");
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
        c.onLtr(0, 5, 3, 17, 2, 0.0);   // once: not yet a call (the words have no checked check bits)
        CHECK(c.log.records().empty() && count(c.takeActions(), Action::Retune) == 0, "one word is not a call");
        c.onLtr(0, 3, 3, 99, 2, 0.1);   // other words in between
        c.onLtr(0, 5, 3, 17, 2, 0.3);
        auto a = c.takeActions();
        const Action* r = find(a, Action::Retune);
        const CallRecord* rec = last(c);
        CHECK(rec && rec->kind == SysKind::Ltr && rec->tg == ltr::tgNumber(0, 3, 17) && rec->label == "0-03-017" && r && r->radio == 0 && std::fabs(r->freqHz - 460112500) < 1,
              "the same word twice: talkgroup %s on repeater 5", rec ? rec->label.c_str() : "");
        CHECK(c.log.records().size() == 1, "the other word was heard once only: %zu calls", c.log.records().size());
        c.onLtr(0, 5, 3, 17, 2, 0.6);
        CHECK(c.log.records().size() == 1 && last(c)->active, "a repeat keeps the call");
        c.onLtr(0, ltr::kTurnOff, 3, 17, 2, 0.8);   // the turn-off
        c.tick(0.81);
        a = c.takeActions();
        CHECK(!last(c)->active && count(a, Action::ReleaseRadio) == 1, "the turn-off word ends the call at once");
        c.onLtr(0, 4, 3, 18, 2, 1.0); c.onLtr(0, 4, 3, 18, 2, 1.2);   // repeater 4 is not in the table
        CHECK(last(c)->note.find("not in your channel list") != std::string::npos, "repeater 4 missing: %s", last(c)->note.c_str());
        c.onLtr(0, 5, 3, 254, 2, 1.3); c.onLtr(0, 5, 3, 254, 2, 1.4);   // a callsign ID word is no call
        c.onLtr(0, 5, 3, 255, 2, 1.3); c.onLtr(0, 5, 3, 255, 2, 1.4);   // idle
        CHECK(c.log.records().size() == 2, "ids 254 / 255 are no calls (%zu records)", c.log.records().size());
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
