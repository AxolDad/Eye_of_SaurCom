// Find-this-radio state machine, the new-radio detector and their helpers (core/include/dect2/source/radio_watch.h). No device is opened here.
#include "dect2/source/radio_watch.h"
#include "dect2/source/usb_diag.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace dect2 {

std::vector<DeviceInfo> watchableRadios(const std::vector<DeviceInfo>& devs) {
    std::vector<DeviceInfo> out;
    for (const auto& d : devs)
        if (d.isRadio() && !d.isNetwork() && d.kind != DeviceInfo::Hamlib) out.push_back(d);
    return out;
}

namespace {
bool samePlace(const DeviceInfo& a, const DeviceInfo& b) {
    return a.kind == b.kind && a.board == b.board && a.serial == b.serial && a.usbPath == b.usbPath;
}
// the key a radio has inside a list (radioKey() alone does not know about repeated serials)
std::string keyIn(const std::vector<RadioRow>& rows, const DeviceInfo& d) {
    for (const auto& r : rows) if (samePlace(r.dev, d) && r.dev.name == d.name) return r.key;
    for (const auto& r : rows) if (samePlace(r.dev, d)) return r.key;
    return radioKey(d);
}
}  // namespace

RadioDiff diffRadiosSettled(const std::vector<DeviceInfo>& before, const std::vector<DeviceInfo>& after) {
    RadioDiff d = diffRadios(before, after);
    for (size_t i = 0; i < d.removed.size();) {
        auto it = std::find_if(d.added.begin(), d.added.end(), [&](const DeviceInfo& a) { return samePlace(a, d.removed[i]); });
        if (it == d.added.end()) { i++; continue; }
        d.added.erase(it);
        d.removed.erase(d.removed.begin() + (long)i);
    }
    return d;
}

std::string radioUsbSignature(const std::vector<UsbNode>& nodes) {
    std::vector<std::string> v;
    for (const auto& n : nodes) {
        if (!usbIsKnownRadio(n.vid, n.pid)) continue;
        char b[48];
        snprintf(b, sizeof b, "%04x:%04x@", (unsigned)n.vid, (unsigned)n.pid);
        v.push_back(std::string(b) + n.path);
    }
    std::sort(v.begin(), v.end());
    std::string s;
    for (const auto& x : v) s += x + ";";
    return s;
}

namespace { std::string tagOfName(const std::string& n); }

std::vector<DeviceInfo> usbRadioDevices(const std::vector<UsbNode>& nodes) {
    std::vector<DeviceInfo> out;
    for (const auto& n : nodes) {
        const char* nm = usbRadioName(n.vid, n.pid);
        if (!nm) continue;
        DeviceInfo d;
        d.kind = (n.vid == 0x1d50 && (n.pid == 0x6089 || n.pid == 0xcc15 || n.pid == 0x604b)) ? DeviceInfo::HackRF : DeviceInfo::Native;
        d.board = tagOfName(nm);
        d.name = nm;
        d.usbPath = n.path;
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(), [](const DeviceInfo& a, const DeviceInfo& b) { return a.usbPath < b.usbPath; });
    return out;
}

namespace {
std::string lowerS(const std::string& s) { std::string t = s; for (auto& c : t) c = (char)tolower((unsigned char)c); return t; }
// The kind id of a walk radio: the board string its listing carries ("rtlsdr", "airspyhf", "lime", ...); HackRF listings say "HackRF One", "HackRF Pro".
std::string tagOfName(const std::string& n) {
    const std::string l = lowerS(n);
    if (l.rfind("hackrf", 0) == 0) return "hackrf";
    if (l.rfind("rtl-sdr", 0) == 0) return "rtlsdr";
    if (l.rfind("airspy hf", 0) == 0) return "airspyhf";
    if (l.rfind("airspy", 0) == 0) return "airspy";
    if (l.rfind("bladerf", 0) == 0) return "bladerf";
    if (l.rfind("limesdr", 0) == 0) return "lime";
    if (l.rfind("plutosdr", 0) == 0) return "pluto";
    if (l.rfind("usrp", 0) == 0) return "usrp";
    if (l.rfind("sdrplay", 0) == 0) return "sdrplay";
    if (l.rfind("fobos", 0) == 0) return "fobos";
    if (l.rfind("rx888 / limesdr", 0) == 0) return "rx888|lime";   // 04b4:00f1 is the FX3 firmware of both
    if (l.rfind("rx888", 0) == 0) return "rx888";
    return l;
}
bool boardIs(const std::string& tag, const std::string& lb) {   // does a listed entry's board (lower case) belong to this kind?
    if (tag == "hackrf") return lb.rfind("hackrf", 0) == 0;
    if (tag == "pluto") return lb == "pluto" || lb == "plutosdr";
    if (tag == "usrp") return lb == "usrp" || lb == "uhd";
    if (tag == "lime") return lb == "lime" || lb == "limesdr";
    return lb == tag;
}
bool sameKind(const DeviceInfo& listed, const DeviceInfo& walk) {
    const std::string lb = lowerS(listed.board);
    size_t p = 0;
    const std::string& t = walk.board;
    while (p <= t.size()) {
        const size_t e = t.find('|', p);
        const std::string alt = t.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (!alt.empty() && boardIs(alt, lb)) return true;
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return false;
}
}  // namespace

std::vector<WalkMatch> matchWalkRadios(const std::vector<DeviceInfo>& list, const std::vector<DeviceInfo>& walk) {
    std::vector<WalkMatch> out(walk.size());
    std::vector<bool> usedByPath(list.size(), false);
    for (size_t w = 0; w < walk.size(); w++) {
        if (walk[w].usbPath.empty()) continue;
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].isRadio() && list[i].usbPath == walk[w].usbPath) { out[w] = {WalkMatch::ByPath, (int)i}; usedByPath[i] = true; break; }
    }
    for (size_t w = 0; w < walk.size(); w++) {
        if (out[w].how == WalkMatch::ByPath) continue;
        int unmatchedWalk = 0;   // walk radios of this kind with no entry by port (this one included)
        for (size_t v = 0; v < walk.size(); v++)
            if (out[v].how != WalkMatch::ByPath && walk[v].board == walk[w].board) unmatchedWalk++;
        std::vector<int> cand;   // listed entries of this kind that cannot say their port
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].isRadio() && !list[i].isNetwork() && list[i].usbPath.empty() && sameKind(list[i], walk[w])) cand.push_back((int)i);
        if (cand.empty()) continue;
        out[w] = (unmatchedWalk == 1 && cand.size() == 1) ? WalkMatch{WalkMatch::ByKind, cand[0]} : WalkMatch{WalkMatch::Ambiguous, -1};
    }
    return out;
}

WalkMatch matchWalkRadio(const std::vector<DeviceInfo>& list, const std::vector<DeviceInfo>& walk, const DeviceInfo& dev) {
    std::vector<DeviceInfo> w = walk;
    size_t at = w.size();
    for (size_t i = 0; i < w.size(); i++) if (w[i].board == dev.board && w[i].usbPath == dev.usbPath && w[i].name == dev.name) { at = i; break; }
    if (at == w.size()) w.push_back(dev);   // it has left the bus: judged with the ones that are there
    const auto m = matchWalkRadios(list, w);
    return m[at];
}

// ---- RadioFind

void RadioFind::start(const std::vector<DeviceInfo>& current, double now) {
    state_ = WaitUnplug;
    t0_ = now;
    base_ = watchableRadios(current);
    left_ = DeviceInfo(); back_ = DeviceInfo();
    wrong_ = false;
}

void RadioFind::cancel() { if (active()) state_ = Cancelled; }

double RadioFind::remaining(double now) const { return std::max(0.0, kTimeoutS - (now - t0_)); }

void RadioFind::tick(double now) {
    if (active() && now - t0_ >= kTimeoutS) state_ = TimedOut;
}

void RadioFind::feed(const std::vector<DeviceInfo>& current, double now) {
    if (!active()) return;
    tick(now);
    if (!active()) return;
    const std::vector<DeviceInfo> cur = watchableRadios(current);
    const RadioDiff d = diffRadiosSettled(base_, cur);
    if (state_ == WaitUnplug) {
        if (d.removed.size() != 1) return;   // nothing yet, or several at once: wait until exactly one has left. A radio that appears is ignored.
        left_ = d.removed[0];
        leftKey_ = keyIn(identifyRadios(base_, RadioLabels{}), left_);
        base_ = cur;
        state_ = WaitReplug;
        return;
    }
    if (d.added.empty()) return;
    const auto rows = identifyRadios(cur, RadioLabels{});
    const DeviceInfo* pick = nullptr;
    for (const auto& a : d.added) if (keyIn(rows, a) == leftKey_) { pick = &a; break; }
    if (!pick) pick = &d.added[0];
    back_ = *pick;
    wrong_ = keyIn(rows, back_) != leftKey_ && !samePlace(back_, left_);
    state_ = Found;
}

// ---- NewRadioDetector

void NewRadioDetector::start(const std::vector<DeviceInfo>& current, double now) {
    t0_ = now;
    started_ = true;
    prev_ = watchableRadios(current);
    seen_.clear();
    for (const auto& r : identifyRadios(prev_, RadioLabels{})) seen_.insert(r.key);
}

std::vector<DeviceInfo> NewRadioDetector::feed(const std::vector<DeviceInfo>& current, double now) {
    std::vector<DeviceInfo> news;
    if (!started_) { start(current, now); return news; }
    const std::vector<DeviceInfo> cur = watchableRadios(current);
    const RadioDiff d = diffRadiosSettled(prev_, cur);
    const auto rows = identifyRadios(cur, RadioLabels{});
    const bool grace = now - t0_ < kGraceS;
    for (const auto& a : d.added) {
        const std::string k = keyIn(rows, a);
        if (!grace && !seen_.count(k)) news.push_back(a);
        seen_.insert(k);
    }
    for (const auto& r : rows) seen_.insert(r.key);   // every key a present radio has had counts as seen (a key changes when a radio with its serial comes or goes)
    prev_ = cur;
    return news;
}

}  // namespace dect2
