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
        for (char& c : line) if (c == ',' || c == ';' || c == '\t' || c == '=' || c == '\r' || c == '"') c = ' ';
        std::istringstream w(line);
        std::string a, b;
        if (!(w >> a)) continue;
        if (!(w >> b)) { bad++; if (bad <= 3) msgs += "line " + std::to_string(lineNo) + ": no frequency\n"; continue; }
        char* e1 = nullptr;
        char* e2 = nullptr;
        const long num = std::strtol(a.c_str(), &e1, 10);
        double f = std::strtod(b.c_str(), &e2);
        if (*e1 || *e2) {   // a header row ("LCN,Frequency") is not a mistake
            if (n == 0 && bad == 0 && (a.empty() || !std::isdigit((unsigned char)a[0]))) continue;
            bad++; if (bad <= 3) msgs += "line " + std::to_string(lineNo) + ": not a number\n";
            continue;
        }
        if (f < 6000) f *= 1e6;   // MHz
        if (num < 0 || num > maxNumber || !(f > 1e6 && f < 6e9)) {
            bad++;
            if (bad <= 3) msgs += "line " + std::to_string(lineNo) + ": " + (num < 0 || num > maxNumber ? "channel number out of range" : "frequency out of range") + "\n";
            continue;
        }
        t_[(int)num] = f;
        n++;
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

bool EdacsModel::onMessage(uint32_t data28, Grant& g, std::string* text) {
    msgs_++;
    const edacs::Message m = edacs::parse(data28);
    if (text) *text = edacs::describe(m);
    const bool afterIndiv = haveIndiv_;
    haveIndiv_ = false;
    switch (m.kind) {
    case edacs::Message::SystemId: sysId_ = m.sysId; site_ = m.site; return false;
    case edacs::Message::GroupVoice:
        g = Grant();
        g.kind = SysKind::Edacs;
        g.group = true; g.tg = m.group; g.channel = m.lcn; g.freqHz = lcns.freq(m.lcn); g.slot = -1;
        g.emergency = m.emergency; g.provoice = m.digital; g.explicitUpdate = m.update;
        g.label = edacs::afsText(m.group);
        n_++;
        return true;
    case edacs::Message::IndivVoice:
        g = Grant();
        g.kind = SysKind::Edacs;
        g.group = false; g.tg = m.lid; g.channel = m.lcn; g.freqHz = lcns.freq(m.lcn); g.slot = -1; g.provoice = m.digital;
        g.label = "LID " + std::to_string(m.lid);
        lastIndiv_ = g; haveIndiv_ = true;
        n_++;
        return true;
    case edacs::Message::Caller:
        if (!afterIndiv) return false;
        g = lastIndiv_;   // the individual call again, now with its caller
        g.src = m.lid;
        return true;
    default: return false;
    }
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

void Controller::onEdacs(uint32_t data28, double now) {
    onControlSeen(now);
    Grant g;
    std::string text;
    if (edacs.onMessage(data28, g, &text)) dispatch({g}, now);
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
