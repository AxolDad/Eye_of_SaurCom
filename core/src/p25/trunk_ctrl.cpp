// Trunking controller: control channel messages in, calls and follow actions out. Also the DMR Tier III grant model.
#include "dect2/p25/trunk.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace trunk {

void DmrModel::setChannel(int lcn, double hz) { if (hz > 0) lcn_[lcn] = hz; else lcn_.erase(lcn); }
double DmrModel::lcnFreq(int lcn) const { auto it = lcn_.find(lcn); return it == lcn_.end() ? 0 : it->second; }

bool DmrModel::onCsbk(int opcode, int fid, const uint8_t d[8], double, Grant& g) {
    if (fid != 0) return false;   // manufacturer CSBKs (Connect Plus, Capacity Plus and the like are not public): not interpreted
    if (opcode != ops_.tvGrant && opcode != ops_.pvGrant && opcode != ops_.btvGrant) return false;
    g = Grant();
    g.kind = SysKind::Dmr;
    g.dmr = true; g.tdma = true;
    g.channel = (d[0] << 4) | (d[1] >> 4);
    g.slot = (d[1] >> 3) & 1;
    g.emergency = (d[1] >> 1) & 1;   // a voice grant has no privacy bit: encryption shows on the traffic channel
    g.tg = (d[2] << 16) | (d[3] << 8) | d[4];
    g.src = (d[5] << 16) | (d[6] << 8) | d[7];
    g.group = opcode != ops_.pvGrant;
    g.freqHz = lcnFreq(g.channel);
    n_++;
    return true;
}

void NxdnModel::setChannel(int ch, double hz) { if (hz > 0) ch_[ch] = hz; else ch_.erase(ch); }
double NxdnModel::channelFreq(int ch) const { auto it = ch_.find(ch); return it == ch_.end() ? 0 : it->second; }

bool NxdnModel::onAssignment(int callType, int src, int dst, int channel, int ccOption, Grant& g) {
    if (channel < 0 || channel > 1023) return false;
    g = Grant();
    g.kind = SysKind::Nxdn;
    g.channel = channel;
    g.tg = dst; g.src = src;
    g.group = callType == 0 || callType == 1;
    g.emergency = (ccOption & 0x80) != 0;
    g.slot = -1;
    g.freqHz = channelFreq(channel);
    n_++;
    return true;
}

Controller::Controller() {}

void Controller::pushMsg(const std::string& s) {
    if (s.empty()) return;
    messages.push_back(s);
    if (messages.size() > 200) messages.erase(messages.begin(), messages.end() - 200);
}

void Controller::dispatch(const std::vector<Grant>& gs, double now) {
    for (const Grant& g : gs) {
        TalkgroupList::Decision d = g.group ? tgs.decide(g.tg) : TalkgroupList::Decision();
        Grant gg = g;
        // a talkgroup the user marked as encrypted in the list is treated as encrypted even if the grant does not say so
        if (const TgEntry* e = g.group ? tgs.find(g.tg) : nullptr) if (e->encryptedMode()) gg.encrypted = true;
        follower.onGrant(gg, d, now, log);
    }
}

void Controller::selectSystem(SysKind k) {
    if (curSet_ && k == cur_) return;
    if (curSet_) others_[cur_] = std::move(tgs);
    auto it = others_.find(k);
    if (it != others_.end()) { tgs = std::move(it->second); others_.erase(it); } else tgs = TalkgroupList();
    cur_ = k; curSet_ = true;
}

void Controller::onTsbk(const p25::Tsbk& t, int nac, double now) {
    adopt(SysKind::P25);
    good_++;
    lastGood_ = now;
    follower.onControlSeen(now);
    std::vector<Grant> gs;
    pushMsg(sys.onTsbk(t, nac, now, &gs));
    if (sys.site().valid && sys.site().ccHz > 0) follower.setControlChannel(sys.site().ccHz, sys.controlCandidates());
    const SiteInfo& si = sys.site();
    if (phase2Ready() && (si.wacn != p2Wacn_ || si.sysId != p2Sys_ || si.nac != p2Nac_)) { p2Wacn_ = si.wacn; p2Sys_ = si.sysId; p2Nac_ = si.nac; p2Gen_++; }
    // a Phase 2 grant before the system identity is known would be followed with the wrong scrambling: hold it
    std::vector<Grant> now_;
    for (const Grant& g : gs) {
        if (g.kind == SysKind::P25 && g.tdma && !g.dmr && !phase2Ready()) {
            bool known = false;
            for (P2Pending& p : p2Pending_) if (p.g.tg == g.tg && p.g.group == g.group && std::fabs(p.g.freqHz - g.freqHz) < 1 && p.g.slot == g.slot) { p.g = g; p.t = now; known = true; }
            if (!known) {
                p2Pending_.push_back({g, now});
                pushMsg((g.group ? "TG " : "unit ") + std::to_string(g.tg) + ": Phase 2 call waits for the system ID (WACN, system and NAC from NET_STS_BCST)");
            }
            continue;
        }
        now_.push_back(g);
    }
    if (phase2Ready() && !p2Pending_.empty()) {
        for (const P2Pending& p : p2Pending_) if (now - p.t < 3.0) now_.push_back(p.g);
        p2Pending_.clear();
    }
    for (auto i = p2Pending_.begin(); i != p2Pending_.end();) i = now - i->t > 3.0 ? p2Pending_.erase(i) : std::next(i);
    dispatch(now_, now);
}

void Controller::onTsbkBad(double) { bad_++; }

void Controller::onDmrCsbk(int opcode, int fid, const uint8_t data[8], double now) {
    Grant g;
    adopt(SysKind::Dmr);
    onControlSeen(now);
    if (dmr.onCsbk(opcode, fid, data, now, g)) dispatch({g}, now);
}

void Controller::onNxdnAssignment(int callType, int src, int dst, int channel, int ccOption, double now) {
    Grant g;
    adopt(SysKind::Nxdn);
    onControlSeen(now);
    if (nxdn.onAssignment(callType, src, dst, channel, ccOption, g)) dispatch({g}, now);
}

void Controller::onControlSeen(double now) {
    good_++;
    lastGood_ = now;
    follower.onControlSeen(now);
}

void Controller::tick(double now) { follower.tick(now, log); }

ControllerStatus Controller::status(double now) const {
    ControllerStatus s;
    s.state = follower.state();
    s.tsbkGood = good_; s.tsbkBad = bad_;
    s.crcOkPct = good_ + bad_ ? 100.0 * (double)good_ / (double)(good_ + bad_) : 0;
    s.controlLocked = now - lastGood_ < follower.config().ccLossSec;
    for (const CallRecord& r : log.records()) if (r.active) s.activeCalls++;
    s.missed = follower.missedCalls();
    return s;
}

std::vector<const CallRecord*> Controller::nowTalking() const {
    std::vector<const CallRecord*> v;
    for (const CallRecord& r : log.records()) if (r.active) v.push_back(&r);
    return v;
}

void Controller::clear() {
    sys.clear();
    log.clear();
    follower.reset();
    messages.clear();
    good_ = bad_ = 0;
    lastGood_ = -1e9;
    p2Pending_.clear();
    p2Wacn_ = p2Sys_ = p2Nac_ = -1;
}

} // namespace trunk
} // namespace dect2
