#include "radio_watch.h"
#include "../app.h"
#include "radio_names.h"
#include "receivers.h"
#include "../ui/notices.h"
#include "../ui/radio_name_ui.h"
#include "dect2/engine/device_claims.h"
#include "dect2/source/usb_path.h"
#include <chrono>
#include <future>

using namespace dect2;

namespace {

struct Poll {
    bool listed = false;       // the radios were listed (the USB set changed)
    std::string sig;           // the USB signature seen
    std::vector<DeviceInfo> radios;
    std::string err;
};

double nowS() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::future<Poll> gFut;
bool gInFlight = false;
double gLastLaunch = -1e9;
std::string gLastSig;
bool gHaveSig = false;
std::vector<DeviceInfo> gLastRadios;   // the watchable radios of the last listing
bool gInit = false;
int gOpenFrame = -100;
bool gEepromBusy = false;

RadioFind gFind;
NewRadioDetector gDet;
int gFindRx = 1;
bool gFindHandled = false;
bool gFindOpenReq = false;
RadioFindView gView;
std::string gFlashKey;
double gFlashUntil = 0;

bool samePlace(const DeviceInfo& a, const DeviceInfo& b) {
    return a.kind == b.kind && a.board == b.board && a.serial == b.serial && a.usbPath == b.usbPath && a.name == b.name;
}
int indexOf(const App& a, const DeviceInfo& d) {
    for (int i = 2; i < (int)a.devices.size(); i++) if (samePlace(a.devices[i], d)) return i;
    return -1;
}

bool dropdownOpen() { return ImGui::GetFrameCount() - gOpenFrame <= 1; }

// The radios that are open right now (a receiver or a scan has them): a background listing leaves them alone.
std::vector<DeviceInfo> streamingRadios() {
    std::vector<DeviceInfo> v;
    for (App* r : receiversAll()) {
        if (r->devIdx < 2 || r->devIdx >= (int)r->devices.size()) continue;
        if (!(r->engine.running() || r->scanner.progress().running)) continue;
        if (r->devices[r->devIdx].isRadio()) v.push_back(r->devices[r->devIdx]);
    }
    return v;
}

void launch() {
    const std::string lastSig = gLastSig;
    const bool haveSig = gHaveSig;
    std::vector<DeviceInfo> known = streamingRadios();
    gInFlight = true;
    gFut = std::async(std::launch::async, [lastSig, haveSig, known]() {
        Poll p;
        p.sig = radioUsbSignature(usbEnumerateFresh());   // descriptors only: nothing is opened
        if (!haveSig || p.sig == lastSig) return p;       // the first walk is the baseline; no change, no listing
        RadioListingKnown streaming(known);
        p.radios = listRadios(p.err);
        p.listed = true;
        return p;
    });
}

App* findApp() { return receiverById(gFindRx); }

void pushNewRadioNotice(const DeviceInfo& d) {
    App* sel = receiverSelected();
    if (!sel) return;
    const int idx = indexOf(*sel, d);
    const std::string shown = idx >= 2 ? radioShownName(*sel, idx) : d.name;
    const std::string key = idx >= 2 ? radioKeyAt(*sel, idx) : radioKey(d);
    const std::string devName = d.name;
    std::vector<NoticeButton> b;
    b.push_back({tr("Name it"), [key, devName]() {
        if (App* s = receiverSelected()) { const int i = findRadio(*s, key, devName); if (i >= 2) radioNameRequest(i); }
    }, true});
    b.push_back({tr("Use in new receiver tab"), [key, devName]() {
        std::string why;
        App* n = addReceiverWithRadio(key, devName, &why);
        if (!n) noticePush(why, {}, 6);
        else if (!why.empty()) noticePush(why, {}, 6);
    }, true});
    b.push_back({tr("Dismiss"), nullptr, true});
    noticePush(std::string(tr("New radio: ")) + shown, std::move(b));
}

void selectFound(const RadioFind& f) {
    gFindHandled = true;
    gView = RadioFindView();
    App* a = findApp();
    const DeviceInfo& back = f.returned();
    if (!a) return;
    const int idx = indexOf(*a, back);
    gView.devIdx = idx;
    gView.back = idx >= 2 ? radioShownName(*a, idx) : back.name;
    {
        const std::string lk = radioKey(f.left());
        RadioRow lr{f.left(), lk, radionames::labelFor(lk), 0, false};
        gView.left = radioDisplayName(lr);
    }
    if (idx < 2) { gView.note = tr("It is not in this receiver's radio list yet. Press Rescan."); return; }
    const int held = deviceHolder(a->devices[idx]);
    if (a->engine.running()) { gView.note = tr("This receiver is running; stop it to choose the radio."); }
    else if (held && held != a->rxId) { gView.note = std::string(tr("In use by ")) + deviceHolderText(held) + "."; }
    else {
        if (a->devIdx != idx) { a->devIdx = idx; a->tune.biasTee = false; }   // the same as choosing it in the dropdown
        gView.selected = true;
    }
    gFlashKey = radioKeyAt(*a, idx);
    gFlashUntil = nowS() + 3.0;
}

void apply(App& first, Poll& p, double now) {
    gLastSig = p.sig;
    gHaveSig = true;
    if (!p.listed) return;
    const std::vector<DeviceInfo> prev = gLastRadios;
    gLastRadios = watchableRadios(p.radios);
    const RadioDiff d = diffRadiosSettled(prev, gLastRadios);
    if (!d.added.empty() || !d.removed.empty()) {
        // the same update as Rescan, from this listing; each receiver keeps its radio by key, and a radio a receiver has open stays listed
        for (App* r : receiversAll()) refreshDevices(*r, &p.radios, &p.err);
    }
    if (gFind.active()) gFind.feed(p.radios, now);
    for (const DeviceInfo& n : gDet.feed(p.radios, now)) pushNewRadioNotice(n);
    (void)first;
}

}  // namespace

void radioWatchDropdownOpen() { gOpenFrame = ImGui::GetFrameCount(); }
void radioWatchEepromBusy(bool busy) { gEepromBusy = busy; }
const RadioFind& radioFind() { return gFind; }
int radioFindRx() { return gFindRx; }
const RadioFindView& radioFindView() { return gView; }
double radioWatchNow() { return nowS(); }
void radioFindCancel() { gFind.cancel(); }
bool radioFindOpenRequested(int rxId) {
    if (!gFindOpenReq || rxId != gFindRx) return false;
    gFindOpenReq = false;
    return true;
}
bool radioFlashing(const std::string& key) { return !key.empty() && key == gFlashKey && nowS() < gFlashUntil; }

void radioFindStart(App& a) {
    if (!gInit) { gLastRadios = watchableRadios(a.devices); gDet.start(gLastRadios, nowS()); gInit = true; }
    gFindRx = a.rxId;
    gLastRadios = watchableRadios(a.devices);   // what the list shows now (a Rescan may have changed it since the last poll)
    gView = RadioFindView();
    gFindHandled = false;
    gFindOpenReq = true;
    gFind.start(gLastRadios, nowS());
    gLastLaunch = -1e9;   // look now
}

void radioWatchTick(App& first) {
    const double now = nowS();
    if (!gInit) { gLastRadios = watchableRadios(first.devices); gDet.start(gLastRadios, now); gInit = true; }
    if (gFind.active()) gFind.tick(now);
    if (gFind.state() == RadioFind::Found && !gFindHandled) selectFound(gFind);
    if (gInFlight) {
        // a result that arrives while the dropdown is open waits: the list must not move under the user's mouse
        if (gFut.valid() && gFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready && !dropdownOpen()) {
            Poll p = gFut.get();
            gInFlight = false;
            apply(first, p, now);
            if (gFind.state() == RadioFind::Found && !gFindHandled) selectFound(gFind);
        }
        return;
    }
    if (dropdownOpen() || gEepromBusy) return;
    if (now - gLastLaunch < (gFind.active() ? 1.0 : 2.0)) return;
    gLastLaunch = now;
    launch();
}
