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

std::vector<DeviceInfo> usbRadioDevices(const std::vector<UsbNode>& nodes) {
    std::vector<DeviceInfo> out;
    for (const auto& n : nodes) {
        const char* nm = usbRadioName(n.vid, n.pid);
        if (!nm) continue;
        DeviceInfo d;
        d.kind = (n.vid == 0x1d50 && (n.pid == 0x6089 || n.pid == 0xcc15 || n.pid == 0x604b)) ? DeviceInfo::HackRF : DeviceInfo::Native;
        d.board = nm;
        d.name = nm;
        d.usbPath = n.path;
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(), [](const DeviceInfo& a, const DeviceInfo& b) { return a.usbPath < b.usbPath; });
    return out;
}

namespace {
// "RTL-SDR" -> "rtls", "SDRplay RSP" -> "sdrp": the first four letters of the id table's name, compared with a listed entry's board and name
std::string kindToken(const std::string& board) {
    std::string t;
    for (unsigned char c : board) { if (isalnum(c)) t += (char)tolower(c); if (t.size() == 4) break; }
    return t;
}
std::string alnumLower(const std::string& s) {
    std::string t;
    for (unsigned char c : s) if (isalnum(c)) t += (char)tolower(c);
    return t;
}
bool sameKind(const DeviceInfo& listed, const DeviceInfo& walk) {
    const std::string t = kindToken(walk.board);
    return !t.empty() && alnumLower(listed.board + " " + listed.name).find(t) != std::string::npos;
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
            if (out[v].how != WalkMatch::ByPath && kindToken(walk[v].board) == kindToken(walk[w].board)) unmatchedWalk++;
        std::vector<int> cand;   // listed entries of this kind that cannot say their port
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].isRadio() && !list[i].isNetwork() && list[i].usbPath.empty() && sameKind(list[i], walk[w])) cand.push_back((int)i);
        if (cand.empty()) continue;
        out[w] = (unmatchedWalk == 1 && cand.size() == 1) ? WalkMatch{WalkMatch::ByKind, cand[0]} : WalkMatch{WalkMatch::Ambiguous, -1};
    }
    return out;
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
