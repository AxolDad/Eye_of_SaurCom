// Analog trunked systems for the shared follower (trunk.h): the user's channel tables, and EDACS, LTR and MPT 1327 control data into grants.
#include "dect2/edacs/edacs_proto.h"
#include "dect2/ltr/ltr_proto.h"
#include <algorithm>
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
    int n = 0, bad = 0, lineNo = 0, single = 0;
    std::string msgs;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        lineNo++;
        if (const size_t h = line.find('#'); h != std::string::npos) line.erase(h);
        if (!line.empty() && (unsigned char)line[0] == 0xEF && line.size() >= 3) line.erase(0, 3);   // a UTF-8 byte order mark
        const bool pairs = line.find('=') != std::string::npos;   // "1=851.0125 2=851.2625": several pairs on a line
        for (char& c : line) if (c == ',' || c == ';' || c == '\t' || c == '=' || c == '\r' || c == '"') c = ' ';
        // "851.0125MHz", "851.0125 MHz": the unit is dropped
        for (size_t u = 0; u + 3 <= line.size(); u++)
            if (std::tolower((unsigned char)line[u]) == 'm' && std::tolower((unsigned char)line[u + 1]) == 'h' && std::tolower((unsigned char)line[u + 2]) == 'z') line.replace(u, 3, "   ");
        {   // a single column of frequencies (a RadioReference site listing): channel numbers 1, 2, 3 ... in order
            std::istringstream one(line);
            std::string tok, more;
            if (one >> tok && !(one >> more) && tok.find('.') != std::string::npos) {
                char* e = nullptr;
                double f = std::strtod(tok.c_str(), &e);
                if (!*e) {
                    if (f < 6000) f *= 1e6;
                    const int num = ++single;
                    if (num <= maxNumber && f > 1e6 && f < 6e9) { t_[num] = f; n++; }
                    else { bad++; if (bad <= 3) msgs += "line " + std::to_string(lineNo) + ": frequency out of range\n"; }
                    continue;
                }
            }
        }
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
    (void)now;
    if (endedTg) *endedTg = -1;
    const int64_t key = ((((int64_t)area * 32 + gotoCh) * 32 + home) * 256 + id) * 32 + freeCh;
    const bool repeated = key == last_;
    last_ = key;
    if (!ltr::isCallId(id) || home < 1 || home > 20) return false;
    if (gotoCh == ltr::kTurnOff) { if (endedTg) *endedTg = ltr::tgNumber(area, home, id); return false; }
    if (gotoCh < 1 || gotoCh > 20 || freeCh > 20) return false;   // LTRStandardMessageProcessor: a call has go-to 1 - 20, free 0 - 20
    g = Grant();
    g.kind = SysKind::Ltr;
    g.group = true; g.tg = ltr::tgNumber(area, home, id); g.channel = gotoCh; g.freqHz = repeaters.freq(gotoCh); g.slot = -1;
    g.explicitUpdate = true;   // every word repeats: a running call is only refreshed
    g.label = ltr::tgText(g.tg);
    g.noPreempt = !repeated;
    n_++;
    return true;
}

bool LtrModel::onPassport(int dcc, int lcn, int site, int group, int type, int freeCh, double now, Grant& g, int* endedTg) {
    (void)now;
    if (endedTg) *endedTg = -1;
    const int64_t key = (((((int64_t)1 << 40) | ((int64_t)dcc << 38)) | ((int64_t)lcn << 27) | ((int64_t)site << 20)) ^ ((int64_t)group << 4) ^ type) * 2048 + freeCh;
    const bool repeated = key == last_;
    last_ = key;
    ltr::PassportWord p;
    p.dcc = dcc; p.lcn = lcn; p.site = site; p.group = group; p.type = type; p.freeCh = freeCh;
    const ltr::PassportKind k = ltr::passportKind(p);
    if (k == ltr::PassportKind::Idle) { band_ = p.siteBand(); return false; }
    if (k == ltr::PassportKind::CallEnd) { if (endedTg) *endedTg = group; return false; }
    if (k != ltr::PassportKind::CallStart && k != ltr::PassportKind::Data) return false;
    if (lcn < 1 || lcn >= 1792) return false;
    g = Grant();
    g.kind = SysKind::Ltr;
    g.group = true; g.tg = group; g.channel = lcn; g.slot = -1;
    g.freqHz = repeaters.freq(lcn);
    if (g.freqHz <= 0 && band_ >= 0) g.freqHz = ltr::passportFreqHz(band_, lcn);
    g.dataCall = k == ltr::PassportKind::Data;
    g.explicitUpdate = true;
    g.label = std::to_string(group);
    g.noPreempt = !repeated;
    n_++;
    return true;
}

std::vector<CoverageGroup> planCoverage(std::vector<double> f, double span, double mainOff, double guard) {
    std::sort(f.begin(), f.end());
    f.erase(std::unique(f.begin(), f.end(), [](double a, double b) { return std::fabs(a - b) < 1; }), f.end());
    const double half = std::max(0.0, span / 2 - guard);
    auto inside = [&](double x, double mainHz) { return std::fabs(x - (mainHz - mainOff)) <= half + 1; };
    std::vector<CoverageGroup> out;
    size_t i = 0;
    while (i < f.size()) {
        // the lowest repeater left must be in the group; the main channel is the one whose capture holds the most repeaters from it up
        size_t best = i, bestEnd = i + 1;
        for (size_t m = i; m < f.size() && f[m] - f[i] <= 2 * half + std::fabs(mainOff) + 1; m++) {
            if (!inside(f[i], f[m])) continue;
            size_t e = i;
            while (e < f.size() && inside(f[e], f[m])) e++;
            if (e > bestEnd || (e == bestEnd && m == i)) { best = m; bestEnd = e; }
        }
        CoverageGroup g;
        g.mainHz = f[best];
        for (size_t k = i; k < bestEnd; k++) if (k != best) g.others.push_back(f[k]);
        out.push_back(g);
        i = bestEnd;
    }
    return out;
}

void LtrSweep::setPlan(const std::vector<CoverageGroup>& g) {
    bool same = g.size() == plan_.size();
    for (size_t i = 0; same && i < g.size(); i++) same = g[i].mainHz == plan_[i].mainHz && g[i].others == plan_[i].others;
    if (same) return;
    plan_ = g;
    idx_ = -1;
    since_ = -1e9;
}

int LtrSweep::step(double now, bool active, bool following) {
    if (plan_.empty()) return -1;
    if (idx_ < 0) { idx_ = 0; since_ = now; return 0; }
    if (following || active) { since_ = now; return -1; }   // a busy group keeps the receiver: it stops on the active one
    if (plan_.size() < 2 || now - since_ < dwellSec) return -1;
    idx_ = (idx_ + 1) % (int)plan_.size();
    since_ = now;
    return idx_;
}

// ------------------------------------------------------------------------------------------------ MPT 1327

bool MptModel::onMessage(const std::string& type, int pfix, int ident, int caller, int channel, bool dataCall, bool traffic, Grant& g) {
    if (type != "GTC" || traffic || channel < 0 || ident < 0) return false;
    mpt1327::Message m;
    m.called = {pfix, ident};
    m.caller = {mpt1327::userIdent(caller) ? pfix : -1, caller};
    const mpt1327::CallKind k = mpt1327::gtcKind(m);
    g = Grant();
    g.kind = SysKind::Mpt1327;
    g.channel = channel; g.freqHz = channels.freq(channel); g.slot = -1;
    g.dataCall = dataCall;
    auto isGroup = [&](int id) { return id >= groupMin && id <= groupMax; };
    g.src = caller >= 0 ? mpt1327::addressNumber(m.caller) : -1;
    switch (k) {
    case mpt1327::CallKind::AllCall:
        g.group = true; g.tg = mpt1327::addressNumber(m.called); g.label = "all-call (ALLI)";
        break;
    case mpt1327::CallKind::ToPhone:     // IDENT1 a gateway: the radio calling out is IDENT2
        g.group = false; g.tg = mpt1327::addressNumber(m.caller); g.src = -1;
        g.label = "telephone call: " + mpt1327::addressText(m.caller);
        break;
    case mpt1327::CallKind::FromPhone:   // IDENT2 a gateway: the radio called is IDENT1
        g.group = false; g.tg = mpt1327::addressNumber(m.called); g.src = -1;
        g.label = "telephone call: " + mpt1327::addressText(m.called);
        break;
    default:
        {
            // IPFIXI / DUMMYI / TSCI as IDENT1: the user ident is IDENT2
            const bool second = !mpt1327::userIdent(ident) && mpt1327::userIdent(caller);
            const mpt1327::Address& who = second ? m.caller : m.called;
            g.group = isGroup(who.ident); g.tg = mpt1327::addressNumber(who);
            if (second) g.src = -1;
            g.label = mpt1327::addressText(who);
        }
        if (k == mpt1327::CallKind::Interprefix) g.label += " (interprefix)";
        else if (k == mpt1327::CallKind::Include) g.label += " (include)";
        break;
    }
    n_++;
    return true;
}

// ------------------------------------------------------------------------------------------------ controller

void Controller::onEdacs(uint32_t m1, uint32_t m2, double now) {
    adopt(SysKind::Edacs);
    onControlSeen(now);
    Grant g;
    int drop = -1;
    if (edacs.onFrame(m1, m2, g, &drop)) dispatch({g}, now);
    if (drop > 0)   // a console drop: the call on that LCN is over
        for (const CallRecord& r : log.records()) if (r.active && r.kind == SysKind::Edacs && r.channel == drop) follower.onTerminator(r.id, now);
}

void Controller::onLtr(int area, int gotoCh, int home, int id, int freeCh, double now) {
    adopt(SysKind::Ltr);
    onControlSeen(now);
    Grant g;
    int ended = -1;
    if (ltr.onWord(area, gotoCh, home, id, freeCh, now, g, &ended)) dispatch({g}, now);
    if (ended >= 0)
        for (const CallRecord& r : log.records()) if (r.active && r.kind == SysKind::Ltr && r.tg == ended) follower.onTerminator(r.id, now);
}

void Controller::onPassport(int dcc, int lcn, int site, int group, int type, int freeCh, double now) {
    adopt(SysKind::Ltr);
    onControlSeen(now);
    Grant g;
    int ended = -1;
    if (ltr.onPassport(dcc, lcn, site, group, type, freeCh, now, g, &ended)) dispatch({g}, now);
    if (ended >= 0)
        for (const CallRecord& r : log.records()) if (r.active && r.kind == SysKind::Ltr && r.tg == ended) follower.onTerminator(r.id, now);
}

void Controller::onMpt(const std::string& type, int pfix, int ident, int caller, int channel, bool dataCall, bool traffic, double now) {
    adopt(SysKind::Mpt1327);
    onControlSeen(now);
    Grant g;
    if (mpt.onMessage(type, pfix, ident, caller, channel, dataCall, traffic, g)) dispatch({g}, now);
}

} // namespace trunk
} // namespace dect2
