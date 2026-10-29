// Trunking follow logic: grant -> follow -> end -> back to the control channel. No DSP: the host executes the Actions and reports back what it hears.
#include "dect2/p25/trunk.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace dect2 {
namespace trunk {

const char* followStateName(FollowState s) {
    switch (s) {
    case FollowState::NoControl: return "no control channel";
    case FollowState::ControlHold: return "on the control channel";
    case FollowState::Following: return "following calls";
    case FollowState::Hopped: return "left the control channel for a call";
    case FollowState::Returning: return "returning to the control channel";
    case FollowState::Searching: return "looking for the control channel";
    }
    return "?";
}

namespace {
int effPrio(int p) { return p <= 0 ? 11 : p; }   // 1 is the highest; none counts as the lowest
}

void Follower::reset() {
    slots_.clear();
    for (Slot& v : voice_) v = Slot();   // the radios stay linked, their calls are forgotten with the log
    dropped_.clear();
    out_.clear();
    state_ = FollowState::NoControl;
    lastCcSec_ = -1e9;
    missed_ = 0;
    noticed_.clear();
    ccHz_ = 0; radioHz_ = 0; homeHz_ = 0; candIdx_ = 0;
    cands_.clear();
}

void Follower::setControlChannel(double hz, const std::vector<double>& cands) {
    if (hz > 0) ccHz_ = hz;
    cands_ = cands;
    if (ccHz_ > 0 && std::none_of(cands_.begin(), cands_.end(), [&](double c) { return std::fabs(c - ccHz_) < 1; })) cands_.insert(cands_.begin(), ccHz_);
}

bool Follower::inSpan(double f) const {
    if (spanHz_ <= 0 || f <= 0) return false;
    return std::fabs(f - centerHz_) <= spanHz_ / 2 - cfg_.edgeGuardHz;
}

int Follower::activeSlices() const {
    int n = 0;
    for (const Slot& s : slots_) if (s.used && !s.hopped) n++;
    return n;
}

uint64_t Follower::audibleCall() const {
    const Slot* best = nullptr;
    auto consider = [&](const Slot& s) {
        if (!s.used) return;
        if (!best || effPrio(s.prio) < effPrio(best->prio) || (effPrio(s.prio) == effPrio(best->prio) && s.startSec < best->startSec)) best = &s;
    };
    for (const Slot& s : slots_) consider(s);
    for (const Slot& s : voice_) consider(s);
    return best ? best->callId : 0;
}

Follower::Slot* Follower::freeSlot() {
    for (Slot& s : slots_) if (!s.used) return &s;
    if ((int)slots_.size() < std::max(1, cfg_.maxSlices)) { slots_.emplace_back(); return &slots_.back(); }
    return nullptr;
}

void Follower::close(Slot& s, CallLog& log, double now, const char* why) {
    if (!s.used) return;
    const int idx = (int)(&s - slots_.data());
    Action a;
    a.kind = Action::RecordStop; a.callId = s.callId; a.slice = idx;
    if (cfg_.record) out_.push_back(a);
    a.kind = s.hopped ? Action::Notice : Action::CloseSlice;
    if (!s.hopped) out_.push_back(a);
    if (CallRecord* r = log.byId(s.callId)) {
        r->active = false;
        r->durSec = std::max(0.0, std::max(s.lastSec, r->lastSec) - r->startSec);
        if (why && *why && r->note.empty()) r->note = why;
    }
    (void)now;
    s.used = false;
}

void Follower::goHome(double now) {
    const double home = ccHz_ > 0 ? ccHz_ : homeHz_;
    if (home <= 0) { setState(FollowState::NoControl, now); return; }
    Action a;
    a.kind = Action::Retune; a.freqHz = home;
    out_.push_back(a);
    radioHz_ = home;
    setState(FollowState::Returning, now);
}

void Follower::onControlSeen(double now) {
    lastCcSec_ = now;
    switch (state_) {
    case FollowState::NoControl:
        if (ccHz_ <= 0 && centerHz_ > 0) ccHz_ = centerHz_;   // DMR / NXDN name no control channel: the one the radio is on now
        setState(FollowState::ControlHold, now);
        break;
    case FollowState::Returning: setState(FollowState::ControlHold, now); break;
    case FollowState::Searching:
        if (radioHz_ > 0) ccHz_ = radioHz_;
        setState(FollowState::ControlHold, now);
        break;
    default: break;
    }
}

void Follower::onGrant(const Grant& g, const TalkgroupList::Decision& d, double now, CallLog& log) {
    lastNow_ = now;
    // a repeat of a call that is running?
    for (size_t i = log.records().size(); i-- > 0;) {
        CallRecord& r = const_cast<CallRecord&>(log.records()[i]);
        if (!r.active || r.tg != g.tg || r.group != g.group || r.dmr != g.dmr) continue;
        if (g.freqHz > 0 && r.freqHz > 0 && std::fabs(g.freqHz - r.freqHz) > 1 && g.slot == r.slot) {   // the call moved: the old one ends
            r.active = false;
            r.durSec = r.lastSec - r.startSec;
            for (Slot& s : slots_) if (s.used && s.callId == r.id) close(s, log, now, "moved");
            for (size_t k = 0; k < voice_.size(); k++) if (voice_[k].used && voice_[k].callId == r.id) endVoice(voice_[k], (int)k, log, "moved");
            break;
        }
        r.lastSec = now;
        if (g.src >= 0 && r.src < 0) r.src = g.src;
        if (g.emergency) r.emergency = true;
        for (Slot& s : slots_) if (s.used && s.callId == r.id) s.lastSec = now;
        for (Slot& s : voice_) if (s.used && s.callId == r.id) s.lastSec = now;
        return;
    }
    CallRecord rec;
    rec.wallTime = (int64_t)time(nullptr);
    rec.startSec = rec.lastSec = now;
    rec.active = true; rec.dmr = g.dmr; rec.group = g.group; rec.tg = g.tg; rec.src = g.src;
    rec.channel = g.channel; rec.freqHz = g.freqHz; rec.slot = g.slot; rec.emergency = g.emergency; rec.encrypted = g.encrypted;
    const uint64_t id = log.add(rec).id;
    log.trim(2000);
    CallRecord* rp = log.byId(id);   // trim moves records: look it up again
    if (!rp) return;
    auto skip = [&](const std::string& why, bool missed) {
        rp->note = why;
        if (missed) { rp->missed = true; missed_++; }
    };
    if (g.encrypted) { skip("encrypted: not followed", false); return; }
    if (!d.follow) { skip(d.reason, false); return; }
    if (g.freqHz <= 0) { skip("channel frequency not known yet (no identifier update heard)", false); return; }
    if (g.tdma && !g.dmr) { skip("P25 Phase 2 TDMA voice: no Phase 2 demodulator", false); return; }
    const int prio = d.priority;
    auto lowest = [&](bool hoppedOnly, bool anyOnly) -> Slot* {
        Slot* v = nullptr;
        for (Slot& s : slots_) {
            if (!s.used) continue;
            if (hoppedOnly && !s.hopped) continue;
            if (!anyOnly && s.hopped) continue;
            if (!v || effPrio(s.prio) > effPrio(v->prio)) v = &s;
        }
        return v;
    };
    auto announce = [&](const std::string& text) {
        for (const auto& n : noticed_) if (std::fabs(n.first - g.freqHz) < 1 && now - n.second < 60) return;
        noticed_.push_back({g.freqHz, now});
        note(text);
    };
    if (inSpan(g.freqHz)) {
        Slot* s = freeSlot();
        if (!s) {
            Slot* v = lowest(false, false);
            if (v && effPrio(prio) < effPrio(v->prio)) { close(*v, log, now, "preempted by a higher priority call"); s = v; }
        }
        if (!s) { skip("busy: every slice is in use", true); return; }
        s->used = true; s->hopped = false; s->callId = id; s->prio = prio; s->freqHz = g.freqHz; s->startSec = s->lastSec = now;
        Action a;
        a.kind = Action::OpenSlice; a.callId = id; a.slice = (int)(s - slots_.data()); a.freqHz = g.freqHz;
        out_.push_back(a);
        if (cfg_.record) {
            Action rs;
            rs.kind = Action::RecordStart; rs.callId = id; rs.slice = a.slice;
            char b[64];
            snprintf(b, sizeof b, "tg%d", g.tg);
            rs.text = b;
            out_.push_back(rs);
        }
        rp->followed = true;
        if (state_ == FollowState::ControlHold) setState(FollowState::Following, now);
        return;
    }
    // outside the capture with voice radios linked: a free voice radio takes the call, the control radio stays where it is
    if (!voice_.empty()) {
        Slot* s = nullptr;
        for (Slot& v : voice_) if (!v.used) { s = &v; break; }
        if (!s) {   // every voice radio is busy: a higher priority call takes the one with the lowest priority call
            Slot* v = nullptr;
            for (Slot& x : voice_) if (!v || effPrio(x.prio) > effPrio(v->prio)) v = &x;
            if (v && effPrio(prio) < effPrio(v->prio)) { endVoice(*v, (int)(v - voice_.data()), log, "preempted by a higher priority call"); s = v; }
        }
        if (!s) { skip("missed (all voice radios busy)", true); return; }
        const int k = (int)(s - voice_.data());
        s->used = true; s->hopped = false; s->callId = id; s->prio = prio; s->freqHz = g.freqHz; s->startSec = s->lastSec = now;
        Action a;
        a.kind = Action::Retune; a.callId = id; a.freqHz = g.freqHz; a.radio = k;
        out_.push_back(a);
        if (cfg_.record) {
            Action rs;
            rs.kind = Action::RecordStart; rs.callId = id; rs.radio = k;
            char b[64];
            snprintf(b, sizeof b, "tg%d", g.tg);
            rs.text = b;
            out_.push_back(rs);
        }
        rp->followed = true;
        return;
    }
    // outside the capture: the radio has to leave the control channel, which blinds it to every other grant
    if (!cfg_.allowRetune) {
        skip("needs a wider capture or a second radio", true);
        announce("Call on " + std::to_string(g.freqHz / 1e6).substr(0, 9) + " MHz is outside the capture: needs a wider capture or a second radio");
        return;
    }
    if (state_ == FollowState::NoControl || state_ == FollowState::Searching) { skip("not on the control channel", true); return; }
    bool block = false;
    for (const Slot& s : slots_) if (s.used && effPrio(s.prio) <= effPrio(prio)) block = true;   // a call of equal or higher priority is running
    if (block) {
        skip("needs a wider capture or a second radio (a call is running)", true);
        announce("Call on " + std::to_string(g.freqHz / 1e6).substr(0, 9) + " MHz is outside the capture while another call is followed: needs a wider capture or a second radio");
        return;
    }
    for (Slot& s : slots_) if (s.used) close(s, log, now, "preempted by a higher priority call");
    if (homeHz_ <= 0 && ccHz_ <= 0) homeHz_ = centerHz_;
    Slot* s = freeSlot();
    if (!s) return;
    s->used = true; s->hopped = true; s->callId = id; s->prio = prio; s->freqHz = g.freqHz; s->startSec = s->lastSec = now;
    Action a;
    a.kind = Action::Retune; a.callId = id; a.freqHz = g.freqHz;
    out_.push_back(a);
    radioHz_ = g.freqHz;
    if (cfg_.record) {
        Action rs;
        rs.kind = Action::RecordStart; rs.callId = id; rs.slice = (int)(s - slots_.data());
        char b[64];
        snprintf(b, sizeof b, "tg%d", g.tg);
        rs.text = b;
        out_.push_back(rs);
    }
    rp->followed = true;
    setState(FollowState::Hopped, now);
}

void Follower::onVoice(uint64_t callId, double now) {
    lastNow_ = now;
    for (Slot& s : slots_) if (s.used && s.callId == callId) s.lastSec = now;
    for (Slot& s : voice_) if (s.used && s.callId == callId) s.lastSec = now;
}

void Follower::onTerminator(uint64_t callId, double now) {
    lastNow_ = now;
    for (Slot& s : slots_) if (s.used && s.callId == callId) s.lastSec = now - cfg_.hangSec - 0.001;   // ends at the next tick
    for (Slot& s : voice_) if (s.used && s.callId == callId) s.lastSec = now - cfg_.hangSec - 0.001;
}

void Follower::tick(double now, CallLog& log) {
    lastNow_ = now;
    bool hoppedEnded = false;
    for (Slot& s : slots_) {
        if (!s.used) continue;
        if (now - s.lastSec > cfg_.hangSec) {
            if (s.hopped) hoppedEnded = true;
            close(s, log, now, "");
        }
    }
    for (auto& d : dropped_) endVoice(d.first, -1, log, d.second.c_str());   // calls on radios that went away
    dropped_.clear();
    for (size_t k = 0; k < voice_.size(); k++) if (voice_[k].used && now - voice_[k].lastSec > cfg_.hangSec) endVoice(voice_[k], (int)k, log, "");
    // calls nobody follows end by silence on the control channel too
    for (const CallRecord& cr : log.records()) {
        if (!cr.active) continue;
        bool owned = false;
        for (const Slot& s : slots_) if (s.used && s.callId == cr.id) owned = true;
        for (const Slot& s : voice_) if (s.used && s.callId == cr.id) owned = true;
        if (!owned && now - cr.lastSec > cfg_.hangSec) {
            CallRecord* r = log.byId(cr.id);
            if (r) { r->active = false; r->durSec = r->lastSec - r->startSec; }
        }
    }
    if (hoppedEnded) { goHome(now); return; }
    switch (state_) {
    case FollowState::Following:
        if (activeSlices() == 0) setState(FollowState::ControlHold, now);
        break;
    case FollowState::ControlHold:
        if (activeSlices() > 0) { setState(FollowState::Following, now); break; }
        if (cfg_.allowRetune && !cands_.empty() && now - lastCcSec_ > cfg_.ccLossSec) {
            candIdx_ = 0;
            Action a;
            a.kind = Action::Retune; a.freqHz = cands_[0];
            out_.push_back(a);
            radioHz_ = cands_[0];
            dwellStart_ = now;
            setState(FollowState::Searching, now);
            note("Control channel lost: looking for it");
        }
        break;
    case FollowState::Returning:
        if (now - stateSince_ > cfg_.returnTimeoutSec) {
            if (cfg_.allowRetune && !cands_.empty()) {
                candIdx_ = 0;
                Action a;
                a.kind = Action::Retune; a.freqHz = cands_[0];
                out_.push_back(a);
                radioHz_ = cands_[0];
                dwellStart_ = now;
                setState(FollowState::Searching, now);
            } else setState(FollowState::NoControl, now);
        }
        break;
    case FollowState::Searching:
        if (!cands_.empty() && now - dwellStart_ > cfg_.ccDwellSec) {
            candIdx_ = (candIdx_ + 1) % cands_.size();
            Action a;
            a.kind = Action::Retune; a.freqHz = cands_[candIdx_];
            out_.push_back(a);
            radioHz_ = cands_[candIdx_];
            dwellStart_ = now;
        }
        break;
    default: break;
    }
}

void Follower::setVoiceRadios(int n) {
    n = std::clamp(n, 0, 16);
    while ((int)voice_.size() > n) {
        if (voice_.back().used) dropped_.push_back({voice_.back(), "voice radio removed"});
        voice_.pop_back();
    }
    voice_.resize((size_t)n);   // new radios start free
}

void Follower::dropVoiceRadio(int k, const char* why) {
    if (k < 0 || k >= (int)voice_.size()) return;
    if (voice_[(size_t)k].used) dropped_.push_back({voice_[(size_t)k], why ? why : ""});
    voice_.erase(voice_.begin() + k);
}

uint64_t Follower::voiceCall(int k) const {
    if (k < 0 || k >= (int)voice_.size() || !voice_[(size_t)k].used) return 0;
    return voice_[(size_t)k].callId;
}

int Follower::voiceRadioOf(uint64_t callId) const {
    if (!callId) return -1;
    for (size_t k = 0; k < voice_.size(); k++) if (voice_[k].used && voice_[k].callId == callId) return (int)k;
    return -1;
}

// The call on a voice radio ends: its recording stops, the host frees the radio (radio < 0: the radio is already gone, nothing to free)
void Follower::endVoice(Slot& s, int radio, CallLog& log, const char* why) {
    if (!s.used) return;
    Action a;
    a.kind = Action::RecordStop; a.callId = s.callId; a.radio = radio;
    if (cfg_.record) out_.push_back(a);
    if (radio >= 0) { a.kind = Action::ReleaseRadio; out_.push_back(a); }
    if (CallRecord* r = log.byId(s.callId)) {
        r->active = false;
        r->durSec = std::max(0.0, std::max(s.lastSec, r->lastSec) - r->startSec);
        if (why && *why && r->note.empty()) r->note = why;
    }
    s.used = false;
}

std::vector<Action> Follower::takeActions() {
    std::vector<Action> o;
    o.swap(out_);
    return o;
}

} // namespace trunk
} // namespace dect2
