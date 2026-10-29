// Analog trunked systems for the shared follower (trunk.h): the user's channel tables, and EDACS, LTR and MPT 1327 control data into grants.
#include "dect2/edacs/edacs_proto.h"
#include "dect2/ltr/ltr_proto.h"
#include "dect2/mpt1327/mpt1327_proto.h"
#include "dect2/p25/trunk.h"
#include <cctype>
#include <cmath>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {
namespace trunk {

const char* sysKindName(SysKind k) {
    switch (k) {
    case SysKind::P25: return "P25";
    case SysKind::Dmr: return "DMR";
    case SysKind::Nxdn: return "NXDN";
    case SysKind::Edacs: return "EDACS";
    case SysKind::Ltr: return "LTR";
    case SysKind::Mpt1327: return "MPT1327";
    }
    return "?";
}

// ------------------------------------------------------------------------------------------------ channel table

int ChannelTable::parse(const std::string& text, int maxNumber, std::string* err, int* skipped) {
    t_.clear();
    int n = 0, bad = 0, lineNo = 0;
    std::string msgs;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        lineNo++;
        if (const size_t h = line.find('#'); h != std::string::npos) line.erase(h);
        if (!line.empty() && (unsigned char)line[0] == 0xEF && line.size() >= 3) line.erase(0, 3);   // a UTF-8 byte order mark
        const bool pairs = line.find('=') != std::string::npos;   // "1=851.0125 2=851.2625": several pairs on a line
        for (char& c : line) if (c == ',' || c == ';' || c == '\t' || c == '=' || c == '\r' || c == '"') c = ' ';
        std::istringstream w(line);
        std::string a, b;
        bool first = true;
        while (w >> a) {
            const bool firstOfLine = first;
            first = false;
            if (!(w >> b)) { bad++; if (bad <= 3) msgs += "line " + std::to_string(lineNo) + ": no frequency\n"; break; }
            char* e1 = nullptr;
            char* e2 = nullptr;
            const long num = std::strtol(a.c_str(), &e1, 10);
            double f = std::strtod(b.c_str(), &e2);
            if (*e1 || *e2) {   // a header row ("LCN,Frequency") is not a mistake
                if (!(firstOfLine && n == 0 && bad == 0 && (a.empty() || !std::isdigit((unsigned char)a[0])))) { bad++; if (bad <= 3) msgs += "line " + std::to_string(lineNo) + ": not a number\n"; }
                break;
            }
            if (f < 6000) f *= 1e6;   // MHz
            if (num < 0 || num > maxNumber || !(f > 1e6 && f < 6e9)) {
                bad++;
                if (bad <= 3) msgs += "line " + std::to_string(lineNo) + ": " + (num < 0 || num > maxNumber ? "channel number out of range" : "frequency out of range") + "\n";
            } else {
                t_[(int)num] = f;
                n++;
            }
            if (!pairs) break;   // "LCN,frequency,anything else": the rest of the line is the user's
        }
    }
    if (err) *err = msgs;
    if (skipped) *skipped = bad;
    return n;
}

std::string ChannelTable::csv() const {
    std::string o;
    char b[64];
    for (const auto& kv : t_) { snprintf(b, sizeof b, "%d,%.5f\n", kv.first, kv.second / 1e6); o += b; }
    return o;
}

double ChannelTable::freq(int n) const {
    const auto it = t_.find(n);
    if (it != t_.end()) return it->second;
    if (base_ > 0 && spacing_ > 0 && n >= 0) return base_ + n * spacing_;
    return 0;
}

// ------------------------------------------------------------------------------------------------ EDACS

bool EdacsModel::onFrame(uint32_t m1, uint32_t m2, Grant& g, int* dropLcn, std::string* text) {
    msgs_++;
    if (dropLcn) *dropLcn = -1;
    const edacs::Mode md = mode == 1 ? edacs::Mode::Extended : edacs::Mode::Standard;
    const edacs::Message m = edacs::parse(m1, m2, md, esk ? edacs::kEsk : 0);
    if (text) *text = edacs::describe(m, md);
    using M = edacs::Message;
    switch (m.kind) {
    case M::SiteId: if (m.site >= 0) site_ = m.site; return false;
    case M::SystemInfo: if (m.sysId > 0) sysId_ = m.sysId; return false;
    case M::UnkeyDrop: if (m.drop && dropLcn) *dropLcn = m.lcn; return false;
    case M::GroupVoice: case M::ChannelUpdate: case M::IndivVoice: case M::AllCall: case M::Interconnect: case M::DataCall: break;
    default: return false;
    }
    if (m.lcn < 1 || m.lcn > 25) return false;   // 0: none; 26..31: a status (queued, busy, denied ...), not a channel
    g = Grant();
    g.kind = SysKind::Edacs;
    g.channel = m.lcn; g.freqHz = lcns.freq(m.lcn); g.slot = -1;
    g.provoice = m.digital; g.emergency = m.emergency; g.explicitUpdate = m.update;
    g.dataCall = m.kind == M::DataCall;
    g.src = m.source;
    if (m.individual || m.kind == M::IndivVoice) {
        g.group = false; g.tg = m.target;
        g.label = "LID " + std::to_string(m.target);
    } else {
        g.group = true; g.tg = m.kind == M::AllCall ? 0 : m.group;
        g.label = m.kind == M::AllCall ? std::string("all-call") : md == edacs::Mode::Standard ? edacs::afsText(m.group) : std::to_string(m.group);
    }
    n_++;
    return true;
}

// ------------------------------------------------------------------------------------------------ LTR

bool LtrModel::onWord(int area, int gotoCh, int home, int id, int freeCh, double now, Grant& g, int* endedTg) {
    (void)freeCh;
    if (endedTg) *endedTg = -1;
    if (!ltr::isCallId(id) || home < 1 || home > 20) return false;
    if (gotoCh == ltr::kTurnOff) { if (endedTg) *endedTg = ltr::tgNumber(area, home, id); return false; }
    if (gotoCh < 1 || gotoCh > 20) return false;
    const int key = ((area * 32 + gotoCh) * 32 + home) * 256 + id;
    const auto it = seen_.find(key);
    const bool twice = it != seen_.end() && now - it->second < 2.0;
    seen_[key] = now;
    if (seen_.size() > 512) for (auto i = seen_.begin(); i != seen_.end();) i = now - i->second > 2.0 ? seen_.erase(i) : std::next(i);
    if (!twice) return false;
    g = Grant();
    g.kind = SysKind::Ltr;
    g.group = true; g.tg = ltr::tgNumber(area, home, id); g.channel = gotoCh; g.freqHz = repeaters.freq(gotoCh); g.slot = -1;
    g.explicitUpdate = true;   // every word repeats: a running call is only refreshed
    g.label = ltr::tgText(g.tg);
    n_++;
    return true;
}

// ------------------------------------------------------------------------------------------------ MPT 1327

bool MptModel::onMessage(const std::string& type, int pfix, int ident, int caller, int channel, bool dataCall, bool traffic, Grant& g) {
    if (type != "GTC" || traffic || channel < 0 || ident < 0) return false;
    g = Grant();
    g.kind = SysKind::Mpt1327;
    mpt1327::Address a; a.pfix = pfix; a.ident = ident;
    g.group = true; g.tg = mpt1327::addressNumber(a); g.src = caller; g.channel = channel; g.freqHz = channels.freq(channel); g.slot = -1;
    g.dataCall = dataCall;
    g.label = mpt1327::addressText(a);
    n_++;
    return true;
}

// ------------------------------------------------------------------------------------------------ controller

void Controller::onEdacs(uint32_t m1, uint32_t m2, double now) {
    onControlSeen(now);
    Grant g;
    int drop = -1;
    if (edacs.onFrame(m1, m2, g, &drop)) dispatch({g}, now);
    if (drop > 0)   // a console drop: the call on that LCN is over
        for (const CallRecord& r : log.records()) if (r.active && r.kind == SysKind::Edacs && r.channel == drop) follower.onTerminator(r.id, now);
}

void Controller::onLtr(int area, int gotoCh, int home, int id, int freeCh, double now) {
    onControlSeen(now);
    Grant g;
    int ended = -1;
    if (ltr.onWord(area, gotoCh, home, id, freeCh, now, g, &ended)) dispatch({g}, now);
    if (ended >= 0)
        for (const CallRecord& r : log.records()) if (r.active && r.kind == SysKind::Ltr && r.tg == ended) follower.onTerminator(r.id, now);
}

void Controller::onMpt(const std::string& type, int pfix, int ident, int caller, int channel, bool dataCall, bool traffic, double now) {
    onControlSeen(now);
    Grant g;
    if (mpt.onMessage(type, pfix, ident, caller, channel, dataCall, traffic, g)) dispatch({g}, now);
}

} // namespace trunk
} // namespace dect2
