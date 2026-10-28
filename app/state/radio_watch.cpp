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
    std::vector<UsbNode> nodes;   // the USB walk (descriptors only)
    std::string sig;              // its radio signature
    bool listed = false;          // the radios were listed too (only when nothing was claimed)
    bool deferred = false;        // a listing was wanted but a radio got claimed first: nothing was opened
    std::vector<DeviceInfo> radios;
    std::string err;
};

double nowS() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::future<Poll> gFut;
bool gInFlight = false;
bool gStop = false;                      // after radioWatchShutdown(): no more polls
double gLastLaunch = -1e9;
std::string gLastSig;
bool gHaveSig = false;
std::vector<DeviceInfo> gLastWalk;       // the radios on the bus at the last walk, made from ids and ports (usbRadioDevices)
bool gInit = false;
double gT0 = 0;                          // the first tick: the new-radio grace counts from here
bool gListPending = false;               // the bus changed and the radios have not been listed since
std::vector<DeviceInfo> gPendingNew;     // new radios (from the walk) waiting for the listing to say what they are
int gOpenFrame = -100;
bool gEepromBusy = false;

RadioFind gFind;
NewRadioDetector gDet;
int gFindRx = 1;
bool gFindHandled = false;
bool gFindOpenReq = false;
bool gFindNeedBase = false;              // Find was started before the first walk: its baseline is that walk
bool gLeftCaptured = false;
RadioFindView gView;
std::string gFlashKey;
double gFlashUntil = 0;

// the entry of a receiver's radio list at this USB port (radios only; -1 when none or the port is unknown)
int indexByPath(const App& a, const std::string& path) {
    if (path.empty()) return -1;
    for (int i = 2; i < (int)a.devices.size(); i++) if (a.devices[i].isRadio() && a.devices[i].usbPath == path) return i;
    return -1;
}
std::string atPort(const DeviceInfo& d) { return d.name + " \xC2\xB7 USB " + (d.usbPath.empty() ? std::string("?") : d.usbPath); }

bool dropdownOpen() { return ImGui::GetFrameCount() - gOpenFrame <= 1; }

// May radios be listed (opened) now? The single answer: the device claims, which the transmitter uses too.
bool listingAllowed() { return backgroundListingAllowed(anyDeviceClaimed(), gEepromBusy); }

void launch(bool wantList) {
    gInFlight = true;
    gFut = std::async(std::launch::async, [wantList]() {
        Poll p;
        p.nodes = usbEnumerateFresh();                    // descriptors only: nothing is opened
        p.sig = radioUsbSignature(p.nodes);
        if (!wantList) return p;
        if (!beginBackgroundListing()) { p.deferred = true; return p; }   // checked again here, atomically: a radio is claimed, so nothing is opened
        p.radios = listRadios(p.err);                      // a Start now waits in claimDevice() until this ends
        endBackgroundListing();
        p.listed = true;
        return p;
    });
}

App* findApp() { return receiverById(gFindRx); }

void pushNewRadioNotice(const DeviceInfo& walkDev, int idx, const App& sel, bool claimed) {
    if (idx < 2) {   // the radios could not be listed (claimed) or the drivers did not list it: say what is known
        std::string t = std::string(tr("New radio: ")) + atPort(walkDev);
        t += claimed ? std::string(". ") + tr("Stop the receivers and press Rescan to see its serial and name it.")
                     : std::string(". ") + tr("It is not in the radio list.");
        noticePush(t, {{tr("Dismiss"), nullptr, true}});
        return;
    }
    const std::string shown = radioShownName(sel, idx);
    const std::string key = radioKeyAt(sel, idx);
    const std::string devName = sel.devices[idx].name;
    std::vector<NoticeButton> b;
    b.push_back({tr("Name it"), [key, devName]() {
        if (App* s = receiverSelected()) { const int i = findRadio(*s, key, devName); if (i >= 2) radioNameRequest(i); }
    }, true});
    b.push_back({tr("Use in new receiver tab"), [key, devName]() {
        std::string why;
        App* n = addReceiverWithRadio(key, devName, &why);
        if (!n || !why.empty()) noticePush(why, {}, 6);
    }, true});
    b.push_back({tr("Dismiss"), nullptr, true});
    noticePush(std::string(tr("New radio: ")) + shown, std::move(b));
}

// The radio that came back is known by its USB port; its entry in the receiver's list has the same port.
void handleFound(const RadioFind& f, int idx) {
    gFindHandled = true;
    const std::string left = gView.left;
    gView = RadioFindView();
    gView.left = left;
    App* a = findApp();
    const DeviceInfo& back = f.returned();
    if (!a) return;
    gView.devIdx = idx;
    if (idx < 2) {
        gView.back = atPort(back);
        gView.note = std::string(tr("Found it at USB ")) + (back.usbPath.empty() ? "?" : back.usbPath) + ". " +
                     tr("Press Rescan after stopping receivers to see its serial.");
        return;
    }
    gView.back = radioShownName(*a, idx);
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

// What the radio list can say now about the Found radio and the new ones. fresh: the radios were just listed.
void flush(bool fresh) {
    const bool noListing = !listingAllowed();   // claimed (or the EEPROM tool is busy): no listing is coming
    const bool waiting = gListPending && !fresh && !noListing;   // a listing is on its way: it will say what the radio is
    if (gFind.state() == RadioFind::Found && !gFindHandled) {
        const App* a = findApp();
        const int idx = a ? indexByPath(*a, gFind.returned().usbPath) : -1;
        if (idx >= 2 || !waiting || !a) handleFound(gFind, idx);
    }
    App* sel = receiverSelected();
    if (!sel) return;
    for (size_t i = 0; i < gPendingNew.size();) {
        const int idx = indexByPath(*sel, gPendingNew[i].usbPath);
        if (idx >= 2 || !waiting) { pushNewRadioNotice(gPendingNew[i], idx, *sel, noListing && !fresh); gPendingNew.erase(gPendingNew.begin() + (long)i); }
        else i++;
    }
}

void apply(App& first, Poll& p, double now) {
    const std::vector<DeviceInfo> walk = usbRadioDevices(p.nodes);
    const bool changed = gHaveSig && p.sig != gLastSig;
    if (!gHaveSig) gDet.start(walk, gT0);   // what is plugged in at the first walk is not news
    else for (const DeviceInfo& n : gDet.feed(walk, now)) gPendingNew.push_back(n);
    if (gFindNeedBase) { gFind.start(walk, now); gFindNeedBase = false; }
    else if (gFind.active()) gFind.feed(walk, now);
    if (gFind.state() == RadioFind::WaitReplug && !gLeftCaptured) {   // name the unplugged radio now, before a listing drops it from the list
        gLeftCaptured = true;
        const App* a = findApp();
        const int i = a ? indexByPath(*a, gFind.left().usbPath) : -1;
        gView.left = i >= 2 ? radioShownName(*a, i) : atPort(gFind.left());
    }
    gLastSig = p.sig;
    gHaveSig = true;
    gLastWalk = walk;
    if (changed) gListPending = true;
    if (p.listed) {
        gListPending = false;
        // the same update as Rescan, from this listing; each receiver keeps its radio by key, and a radio a receiver has open stays listed
        for (App* r : receiversAll()) refreshDevices(*r, &p.radios, &p.err);
    }
    flush(p.listed);
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
    gFindRx = a.rxId;
    gView = RadioFindView();
    gFindHandled = false;
    gLeftCaptured = false;
    gFindOpenReq = true;
    gFindNeedBase = !gHaveSig;           // before the first walk: the baseline is that walk
    gFind.start(gLastWalk, nowS());
    gLastLaunch = -1e9;                  // look now
}

void radioWatchShutdown() {
    gStop = true;
    if (gFut.valid()) gFut.wait();       // a worker inside the listing must finish before the radio libraries are torn down
    gFut = std::future<Poll>();
    gInFlight = false;
}

void radioWatchTick(App& first) {
    if (gStop) return;
    const double now = nowS();
    if (!gInit) { gInit = true; gT0 = now; }
    if (gFind.active()) gFind.tick(now);
    if (gInFlight) {
        // a result that arrives while the dropdown is open waits: the list must not move under the user's mouse
        if (gFut.valid() && gFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready && !dropdownOpen()) {
            Poll p = gFut.get();
            gInFlight = false;
            apply(first, p, now);
        }
        return;
    }
    if (gEepromBusy) return;   // the EEPROM tool has a dongle: not even a walk
    if (dropdownOpen()) return;
    if (now - gLastLaunch < (gFind.active() ? 1.0 : 2.0)) return;
    gLastLaunch = now;
    launch(gListPending && listingAllowed());
}
