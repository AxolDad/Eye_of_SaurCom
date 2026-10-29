// Voice radios for the trunking follower (trunk.h, Follower::setVoiceRadios): what a voice radio's receiver hears of the call it was sent
// to. The host reads the voice radio's own telemetry each frame and reports to the Controller: Voice -> onVoice, Ended -> onTerminator.
// Plain logic over the P25, DMR and NXDN telemetry; no DSP.
#pragma once
#include "dect2/dmr/dmr_tel.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "dect2/p25/p25_tel.h"
#include <cstdint>

namespace dect2 {
namespace trunk {

// One per voice radio. A new call id forgets what was heard: a terminator only counts after this call's own voice was heard, so the
// last call left in the receiver's telemetry (same channel, same talkgroup) never ends the new one.
struct VoiceWatch {
    uint64_t callId = 0;
    bool heard = false;
};

enum class VoiceHeard { Nothing, Voice, Ended };

// tg: the call's talkgroup (or called unit); slot: the call's DMR time slot (0 or 1)
VoiceHeard p25VoiceHeard(VoiceWatch& w, uint64_t callId, int tg, const P25Call& now);
VoiceHeard dmrVoiceHeard(VoiceWatch& w, uint64_t callId, int tg, int slot, const DmrSlot slots[2]);
VoiceHeard nxdnVoiceHeard(VoiceWatch& w, uint64_t callId, int tg, const NxdnCall& now);

} // namespace trunk
} // namespace dect2
