// Voice radios for the trunking follower (trunk.h, Follower::setVoiceRadios), as plain logic with no DSP and no UI:
//   - what a voice radio's receiver hears of the call it was sent to (its own telemetry -> Voice keeps the call, Ended is its terminator);
//   - the rules the app follows for linked receivers: who may feed the follower, when a voice radio is unlinked, which receivers may run
//     the same mode at once. app/p25/p25_ui.cpp and app/state/receivers.cpp apply them; tests/p25/test_p25_trunk_voice.cpp tests them.
#pragma once
#include "dect2/dmr/dmr_tel.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "dect2/p25/p25_tel.h"
#include <cstdint>

namespace dect2 {
namespace trunk {

// ------------------------------------------------------------------------------------------------ what a voice radio hears

// One per voice radio, kept across its calls.
//  - seq: the telemetry report last read (the receiver's own sequence number). A report that is not newer is ignored, so the frozen
//    telemetry of a stopped receiver never keeps a call alive or ends it.
//  - heard: this call's own voice was heard (its talkgroup decoded). Only then does a terminator end it, so what the receiver still
//    shows of the previous call (same channel, talkgroup not decoded yet) never ends the new one.
struct VoiceWatch {
    uint64_t callId = 0;
    bool heard = false;
    uint64_t seq = 0;
};

enum class VoiceHeard { Nothing, Voice, Ended };

// seq: the receiver's telemetry sequence number (RxTelemetry::seq); tg: the call's talkgroup (or called unit)
VoiceHeard p25VoiceHeard(VoiceWatch& w, uint64_t callId, int tg, const P25Call& now, uint64_t seq);
VoiceHeard nxdnVoiceHeard(VoiceWatch& w, uint64_t callId, int tg, const NxdnCall& now, uint64_t seq);
// DMR: the call's time slot (0 or 1). Ended only on the Terminator with LC (base stations repeat it through the hang time); the call flag
// clearing for another reason (a fade, a data burst) only stops keeping the call, which then ends on the hang time.
VoiceHeard dmrVoiceHeard(VoiceWatch& w, uint64_t callId, int tg, int slot, const DmrSlot slots[2], uint64_t seq);

// ------------------------------------------------------------------------------------------------ the app's rules for linked receivers

bool isTrunkFamily(int family);   // P25 (50), DMR (9), NXDN (38): the app's mode families the follower serves
// A voice radio's decoder plays only while it carries a followed call (never an unfollowed channel), and never against the user's mute
inline bool voiceRadioMuted(bool userMuted, uint64_t callOnRadio) { return userMuted || callOnRadio == 0; }
// A receiver that takes over the follower from another one (prevOwner != 0) starts from its latest message: what it stored before
// (old grants) is not new. The first owner (prevOwner 0) reads what it holds, as before.
inline bool skipBacklogOnTakeover(int prevOwner, int newOwner) { return prevOwner != 0 && prevOwner != newOwner; }

// What the rules need to know about one receiver tab
struct RxState {
    int rxId = 0;
    int family = -1, sub = 0;
    int voiceFor = 0;            // linked as a voice radio of RX n
    bool running = false;        // its engine runs
    bool claimed = false;        // it holds its radio (device claims), or it uses no radio (test signal, file)
    bool radioLost = false;      // its radio stopped delivering samples (unplugged)
};

// May r feed the follower as its control receiver (control messages, its own voice when it hops)? owner: the receiver that drives it now
// (nullptr none). Only a running receiver in a trunked mode that is not a voice radio; while the owner is still able to, nobody else.
bool controlMayFeed(const RxState& r, const RxState* owner);
// May the voice radio v's telemetry be read for the follower? Linked to ctl, in ctl's mode, running, holding its radio, radio not lost.
bool voiceMayFeed(const RxState& v, const RxState& ctl);

enum class VoiceFate {
    Keep,          // stays linked
    UnlinkKeep,    // the user switched the voice tab to another mode: unlinked, the tab is the user's now
    UnlinkClose,   // stopped, unplugged, or its radio taken: unlinked, the call ends, the tab closes
    Gone,          // the voice tab was closed: unlinked
    AllClose,      // the control receiver is gone, or not in the linked mode any more: every voice radio is unlinked and closed
};
// v: nullptr when the voice tab is closed; ctl: nullptr when the control receiver is closed. linkFamily/linkSub: the mode they were linked in.
// startedOrGraceOver: the voice receiver has run since it was linked, or the time to start it is over.
VoiceFate voiceFate(const RxState* v, const RxState* ctl, int linkFamily, int linkSub, bool startedOrGraceOver);

// receiverModeBusy's exception: a and b (both in the same mode) may run at once only as a linked pair (or two voice radios of one control
// receiver), all in the control receiver's mode. ctl: the control receiver of a's or b's link (nullptr none).
bool linkedPairMayShareMode(const RxState& a, const RxState& b, const RxState* ctl);

} // namespace trunk
} // namespace dect2
