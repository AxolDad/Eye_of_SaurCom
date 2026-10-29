// Voice radios: what a voice radio hears of its call, and the app's rules for linked receivers (trunk_voice.h).
#include "dect2/p25/trunk_voice.h"
#include <cstring>

namespace dect2 {
namespace trunk {

namespace {
// false: nothing to do (no call, or no new report). A new call forgets what was heard; the report sequence is kept.
bool fresh(VoiceWatch& w, uint64_t callId, uint64_t seq) {
    if (w.callId != callId) { w.callId = callId; w.heard = false; }
    if (!callId) return false;
    if (seq != 0 && seq <= w.seq) return false;
    w.seq = seq;
    return true;
}
bool ours(long long dst, int tg) { return dst < 0 || dst == tg; }   // the destination may not be decoded yet
}

VoiceHeard p25VoiceHeard(VoiceWatch& w, uint64_t callId, int tg, const P25Call& n, uint64_t seq) {
    if (!fresh(w, callId, seq) || n.kind != 0 || !ours(n.dst, tg)) return VoiceHeard::Nothing;
    if (n.active) { if (n.dst == tg) w.heard = true; return VoiceHeard::Voice; }
    return w.heard && n.endSeen ? VoiceHeard::Ended : VoiceHeard::Nothing;
}

VoiceHeard nxdnVoiceHeard(VoiceWatch& w, uint64_t callId, int tg, const NxdnCall& n, uint64_t seq) {
    if (!fresh(w, callId, seq) || n.kind != 0 || !ours(n.dst, tg)) return VoiceHeard::Nothing;
    if (n.active) { if (n.dst == tg) w.heard = true; return VoiceHeard::Voice; }
    return w.heard && n.endSeen ? VoiceHeard::Ended : VoiceHeard::Nothing;
}

VoiceHeard dmrVoiceHeard(VoiceWatch& w, uint64_t callId, int tg, int slot, const DmrSlot slots[2], uint64_t seq) {
    if (!fresh(w, callId, seq) || slot < 0 || slot > 1) return VoiceHeard::Nothing;
    const DmrSlot& s = slots[slot];
    const long long dst = s.dst ? (long long)s.dst : -1;   // 0: not decoded yet (and after a call ended)
    if (!ours(dst, tg)) return VoiceHeard::Nothing;
    if (s.active && s.state == 1 && s.inCall) { if (dst == tg) w.heard = true; return VoiceHeard::Voice; }
    if (w.heard && !s.inCall && s.active && s.lastBurst.rfind("Terminator", 0) == 0) return VoiceHeard::Ended;   // Terminator with LC
    return VoiceHeard::Nothing;
}

bool isTrunkFamily(int family) { return family == 50 || family == 9 || family == 38; }

bool controlMayFeed(const RxState& r, const RxState* owner) {
    if (!r.running || r.voiceFor || !isTrunkFamily(r.family)) return false;
    if (!owner || owner->rxId == r.rxId) return true;
    return !(owner->running && !owner->voiceFor && isTrunkFamily(owner->family));   // the owner can no longer: r takes over
}

bool voiceMayFeed(const RxState& v, const RxState& ctl) {
    return v.voiceFor == ctl.rxId && v.family == ctl.family && v.sub == ctl.sub && v.running && v.claimed && !v.radioLost;
}

VoiceFate voiceFate(const RxState* v, const RxState* ctl, int linkFamily, int linkSub, bool startedOrGraceOver) {
    if (!ctl || ctl->family != linkFamily || ctl->sub != linkSub) return VoiceFate::AllClose;   // the control's switch, checked first
    if (!v) return VoiceFate::Gone;
    if (v->family != linkFamily || v->sub != linkSub) return VoiceFate::UnlinkKeep;
    if (v->radioLost || (v->running && !v->claimed)) return VoiceFate::UnlinkClose;
    if (!v->running && startedOrGraceOver) return VoiceFate::UnlinkClose;
    return VoiceFate::Keep;
}

bool linkedPairMayShareMode(const RxState& a, const RxState& b, const RxState* ctl) {
    if (!a.voiceFor && !b.voiceFor) return false;
    if (!ctl || !isTrunkFamily(ctl->family)) return false;
    const int ga = a.voiceFor ? a.voiceFor : a.rxId, gb = b.voiceFor ? b.voiceFor : b.rxId;
    if (ga != ctl->rxId || gb != ctl->rxId) return false;
    return a.family == ctl->family && a.sub == ctl->sub && b.family == ctl->family && b.sub == ctl->sub;
}

} // namespace trunk
} // namespace dect2
