// What a voice radio hears of the call the follower sent it to (trunk_voice.h).
#include "dect2/p25/trunk_voice.h"

namespace dect2 {
namespace trunk {

namespace {
bool fresh(VoiceWatch& w, uint64_t callId) {
    if (w.callId != callId) { w = VoiceWatch(); w.callId = callId; }
    return callId != 0;
}
bool ours(long long dst, int tg) { return dst < 0 || dst == tg; }   // the destination may not be decoded yet
}

VoiceHeard p25VoiceHeard(VoiceWatch& w, uint64_t callId, int tg, const P25Call& n) {
    if (!fresh(w, callId) || n.kind != 0 || !ours(n.dst, tg)) return VoiceHeard::Nothing;
    if (n.active) { w.heard = true; return VoiceHeard::Voice; }
    return w.heard && n.endSeen ? VoiceHeard::Ended : VoiceHeard::Nothing;
}

VoiceHeard dmrVoiceHeard(VoiceWatch& w, uint64_t callId, int tg, int slot, const DmrSlot slots[2]) {
    if (!fresh(w, callId) || slot < 0 || slot > 1) return VoiceHeard::Nothing;
    const DmrSlot& s = slots[slot];
    if (!ours(s.dst ? (long long)s.dst : -1, tg)) return VoiceHeard::Nothing;   // 0: not decoded yet
    if (s.active && s.state == 1) { w.heard = true; return VoiceHeard::Voice; }
    return w.heard && !s.inCall ? VoiceHeard::Ended : VoiceHeard::Nothing;   // the terminator with LC closes the call on the slot
}

VoiceHeard nxdnVoiceHeard(VoiceWatch& w, uint64_t callId, int tg, const NxdnCall& n) {
    if (!fresh(w, callId) || n.kind != 0 || !ours(n.dst, tg)) return VoiceHeard::Nothing;
    if (n.active) { w.heard = true; return VoiceHeard::Voice; }
    return w.heard && n.endSeen ? VoiceHeard::Ended : VoiceHeard::Nothing;
}

} // namespace trunk
} // namespace dect2
