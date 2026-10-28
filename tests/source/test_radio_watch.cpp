// Find-this-radio state machine and the new-radio detector: fed with lists and a clock, no devices.
#include "../common/test_util.h"
#include "dect2/source/radio_watch.h"
#include "dect2/engine/device_claims.h"
#include <atomic>
#include <chrono>
#include <thread>

using namespace dect2;

static DeviceInfo rtl(const char* serial, const char* usb, const char* name = "Generic RTL2832U") {
    DeviceInfo d;
    d.kind = DeviceInfo::Native; d.board = "rtlsdr"; d.name = name; d.serial = serial; d.usbPath = usb;
    return d;
}
static DeviceInfo hackrf(const char* serial, const char* usb) {
    DeviceInfo d;
    d.kind = DeviceInfo::HackRF; d.board = "HackRF One"; d.name = std::string("HackRF One (") + serial + ")"; d.serial = serial; d.usbPath = usb;
    return d;
}
using L = std::vector<DeviceInfo>;

int main() {
    // ---- RadioFind
    {   // unplug detected, replug detected: the same radio comes back
        RadioFind f;
        const DeviceInfo a = rtl("AAA", "1-1"), b = rtl("BBB", "1-2"), c = hackrf("0123456789", "1-3");
        f.start({a, b, c}, 100);
        CHECK(f.state() == RadioFind::WaitUnplug && f.active(), "waits for the unplug");
        f.feed({a, b, c}, 101);
        CHECK(f.state() == RadioFind::WaitUnplug, "nothing left yet");
        f.feed({a, c}, 102);
        CHECK(f.state() == RadioFind::WaitReplug && f.left().serial == "BBB", "unplug detected: %s", f.left().serial.c_str());
        f.feed({a, c}, 103);
        CHECK(f.state() == RadioFind::WaitReplug, "still waiting for the replug");
        f.feed({a, b, c}, 105);
        CHECK(f.state() == RadioFind::Found && f.returned().serial == "BBB" && !f.wrongRadio() && !f.active(), "replug detected");
        f.feed({a, c}, 106);
        CHECK(f.state() == RadioFind::Found, "Found is final");
    }
    {   // the wrong radio comes back (a different radio, not the one that left): found, but flagged and says which one
        RadioFind f;
        const DeviceInfo a = rtl("AAA", "1-1"), b = rtl("BBB", "1-2"), x = rtl("XXX", "1-2");
        f.start({a, b}, 0);
        f.feed({a}, 1);
        CHECK(f.state() == RadioFind::WaitReplug, "b left");
        f.feed({a, x}, 2);
        CHECK(f.state() == RadioFind::Found && f.wrongRadio() && f.returned().serial == "XXX" && f.left().serial == "BBB", "wrong radio flagged: %d %s", (int)f.wrongRadio(), f.returned().serial.c_str());
    }
    {   // identical serials on two ports: the same radio on the other port is a different key
        RadioFind f;
        const DeviceInfo a = rtl("00000001", "1-1"), b = rtl("00000001", "1-2"), b2 = rtl("00000001", "1-4");
        f.start({a, b}, 0);
        f.feed({a}, 1);
        CHECK(f.state() == RadioFind::WaitReplug && f.left().usbPath == "1-2", "b left despite key flip of a: state %d left %s", (int)f.state(), f.left().usbPath.c_str());
        f.feed({a, b2}, 2);
        CHECK(f.state() == RadioFind::Found && f.wrongRadio(), "other port is flagged");
    }
    {   // the same identical-serial radio back in its port is right
        RadioFind f;
        const DeviceInfo a = rtl("00000001", "1-1"), b = rtl("00000001", "1-2");
        f.start({a, b}, 0);
        f.feed({a}, 1);
        f.feed({a, b}, 2);
        CHECK(f.state() == RadioFind::Found && !f.wrongRadio() && f.returned().usbPath == "1-2", "same port is right");
    }
    {   // several radios leave at once: wait until exactly one has left
        RadioFind f;
        const DeviceInfo a = rtl("AAA", "1-1"), b = rtl("BBB", "1-2"), c = rtl("CCC", "1-3");
        f.start({a, b, c}, 0);
        f.feed({a}, 1);
        CHECK(f.state() == RadioFind::WaitUnplug, "two left at once: still waiting");
        f.feed({a, b}, 2);
        CHECK(f.state() == RadioFind::WaitReplug && f.left().serial == "CCC", "b is back, c is the one out: %d", (int)f.state());
    }
    {   // a radio that appears but never left is ignored
        RadioFind f;
        const DeviceInfo a = rtl("AAA", "1-1"), n = rtl("NEW", "1-5"), b = rtl("BBB", "1-2");
        f.start({a, b}, 0);
        f.feed({a, b, n}, 1);
        CHECK(f.state() == RadioFind::WaitUnplug, "new radio ignored while waiting for the unplug");
        f.feed({a, n}, 2);
        CHECK(f.state() == RadioFind::WaitReplug && f.left().serial == "BBB", "b left");
        f.feed({a, n}, 3);
        CHECK(f.state() == RadioFind::WaitReplug, "a radio already present is not a replug");
        f.feed({a, n, b}, 4);
        CHECK(f.state() == RadioFind::Found && f.returned().serial == "BBB" && !f.wrongRadio(), "b back");
    }
    {   // timeout from start: both phases
        RadioFind f;
        const DeviceInfo a = rtl("AAA", "1-1");
        f.start({a}, 10);
        f.tick(69.9);
        CHECK(f.state() == RadioFind::WaitUnplug, "not yet at 59.9 s");
        CHECK(f.remaining(40) > 29.9 && f.remaining(40) < 30.1, "remaining %f", f.remaining(40));
        f.feed({a}, 70.0);
        CHECK(f.state() == RadioFind::TimedOut && !f.active(), "timed out waiting for the unplug");
        RadioFind g;
        const DeviceInfo b = rtl("BBB", "1-2");
        g.start({a, b}, 0);
        g.feed({a}, 5);
        g.tick(59);
        CHECK(g.state() == RadioFind::WaitReplug, "waiting for the replug at 59 s");
        g.tick(60);
        CHECK(g.state() == RadioFind::TimedOut, "timed out waiting for the replug (the 60 s count from start)");
        g.feed({a, b}, 61);
        CHECK(g.state() == RadioFind::TimedOut, "TimedOut is final");
    }
    {   // a replug that arrives after the 60 s is too late
        RadioFind f;
        const DeviceInfo a = rtl("AAA", "1-1"), b = rtl("BBB", "1-2");
        f.start({a, b}, 0);
        f.feed({a}, 30);
        f.feed({a, b}, 61);
        CHECK(f.state() == RadioFind::TimedOut, "late replug is a timeout");
    }
    {   // cancel
        RadioFind f;
        f.start({rtl("AAA", "1-1")}, 0);
        f.cancel();
        CHECK(f.state() == RadioFind::Cancelled && !f.active(), "cancelled");
        f.feed({}, 1);
        CHECK(f.state() == RadioFind::Cancelled, "cancel is final");
        f.start({rtl("AAA", "1-1")}, 2);   // and can start again
        CHECK(f.state() == RadioFind::WaitUnplug, "restart");
    }
    {   // non-radios and network radios in the list do not count
        RadioFind f;
        DeviceInfo synth; synth.kind = DeviceInfo::Synthetic; synth.name = "Test signal";
        DeviceInfo net = rtl("NET", ""); net.network = true;
        const DeviceInfo a = rtl("AAA", "1-1");
        f.start({synth, a, net}, 0);
        f.feed({synth, net}, 1);
        CHECK(f.state() == RadioFind::WaitReplug && f.left().serial == "AAA", "only the usb radio counts");
    }

    // ---- NewRadioDetector
    {   // nothing in the first 5 s, then one notice per new key
        NewRadioDetector d;
        const DeviceInfo a = rtl("AAA", "1-1"), b = rtl("BBB", "1-2"), c = rtl("CCC", "1-3");
        d.start({a}, 100);
        CHECK(d.feed({a, b}, 102).empty(), "start grace: no notice at 2 s");
        CHECK(d.seen(radioKey(b)), "the radio seen in the grace counts as seen");
        auto n = d.feed({a, b, c}, 105.0);
        CHECK(n.size() == 1 && n[0].serial == "CCC", "after the grace: one notice (%zu)", n.size());
        CHECK(d.feed({a, b, c}, 107).empty(), "no repeat");
    }
    {   // two new at once give two notices
        NewRadioDetector d;
        d.start({}, 0);
        auto n = d.feed({rtl("A", "1-1"), rtl("B", "1-2")}, 10);
        CHECK(n.size() == 2, "two notices: %zu", n.size());
    }
    {   // a swap in one poll with the same serial and USB path is the same radio (a key change), no notice
        NewRadioDetector d;
        const DeviceInfo a = rtl("00000001", "1-1");
        d.start({a}, 0);
        // a second radio with the same serial arrives: a's key flips from sn: to usb:; only the new one is news
        auto n = d.feed({a, rtl("00000001", "1-2")}, 10);
        CHECK(n.size() == 1 && n[0].usbPath == "1-2", "only the new radio is news: %zu", n.size());
        // the key flip alone, nothing new: 77 shared by two, one goes: the remaining one flips back
        NewRadioDetector e;
        e.start({rtl("77", "1-1"), rtl("77", "1-2")}, 0);
        CHECK(e.feed({rtl("77", "1-1")}, 10).empty(), "removal and key change: no notice");
        CHECK(e.feed({rtl("77", "1-1"), rtl("77", "1-2")}, 20).empty(), "the one that left comes back: seen before");
        // same serial and path, different poll content (a re-read with a different display name)
        NewRadioDetector g;
        g.start({rtl("S1", "1-1", "Old name")}, 0);
        CHECK(g.feed({rtl("S1", "1-1", "New name")}, 10).empty(), "same radio, different text: nothing");
    }
    {   // a radio that goes away and comes back with the same key gives no notice when seen before
        NewRadioDetector d;
        const DeviceInfo a = rtl("AAA", "1-1"), b = rtl("BBB", "1-2");
        d.start({a, b}, 0);
        CHECK(d.feed({a}, 10).empty(), "b leaves: nothing");
        CHECK(d.feed({a, b}, 12).empty(), "b returns: seen this session, nothing");
        CHECK(d.feed({a, b, rtl("CCC", "1-3")}, 14).size() == 1, "a real new one still gives a notice");
        // a radio that came and went comes again: one notice only the first time
        CHECK(d.feed({a, b}, 16).empty(), "c leaves");
        CHECK(d.feed({a, b, rtl("CCC", "1-3")}, 18).empty(), "c returns: seen");
    }
    {   // network radios and rigs are not plugged-in news
        NewRadioDetector d;
        d.start({}, 0);
        DeviceInfo net = rtl("N", ""); net.network = true;
        DeviceInfo rig; rig.kind = DeviceInfo::Hamlib; rig.name = "rig";
        CHECK(d.feed({net, rig}, 10).empty(), "network radio and rig are left out");
    }

    // ---- helpers
    {
        L v = {rtl("AAA", "1-1")};
        DeviceInfo net = rtl("N", ""); net.network = true;
        DeviceInfo s; s.kind = DeviceInfo::Synthetic;
        v.push_back(net); v.push_back(s);
        CHECK(watchableRadios(v).size() == 1, "watchable");
        const auto d = diffRadiosSettled({rtl("77", "1-1"), rtl("77", "1-2")}, {rtl("77", "1-1")});
        CHECK(d.added.empty() && d.removed.size() == 1 && d.removed[0].usbPath == "1-2", "settled diff: %zu/%zu", d.added.size(), d.removed.size());
    }
    {   // the USB signature: radio ids only, order free, a move to another port changes it
        auto node = [](uint16_t vid, uint16_t pid, const char* path) { UsbNode n; n.vid = vid; n.pid = pid; n.path = path; return n; };
        std::vector<UsbNode> a = {node(0x0bda, 0x2838, "1-1"), node(0x046d, 0xc077, "1-5"), node(0x1d50, 0x6089, "1-2")};
        std::vector<UsbNode> b = {node(0x1d50, 0x6089, "1-2"), node(0x0bda, 0x2838, "1-1")};
        CHECK(radioUsbSignature(a) == radioUsbSignature(b), "a mouse and the order do not matter");
        std::vector<UsbNode> c = {node(0x1d50, 0x6089, "1-2")};
        CHECK(radioUsbSignature(a) != radioUsbSignature(c), "a radio less");
        std::vector<UsbNode> m = {node(0x0bda, 0x2838, "1-4"), node(0x1d50, 0x6089, "1-2")};
        CHECK(radioUsbSignature(b) != radioUsbSignature(m), "another port");
        CHECK(radioUsbSignature({}) == radioUsbSignature({node(0x046d, 0xc077, "1-5")}), "no radios either way");
    }
    {   // Fobos and RX888 are on the bus table
        auto node = [](uint16_t vid, uint16_t pid, const char* path) { UsbNode n; n.vid = vid; n.pid = pid; n.path = path; return n; };
        CHECK(radioUsbSignature({node(0x16d0, 0x132e, "1-1")}) != radioUsbSignature({}), "Fobos in the signature");
        CHECK(radioUsbSignature({node(0x04b4, 0x00f1, "1-1")}) != radioUsbSignature({}), "RX888 in the signature");
    }
    {   // devices made from the walk: keyed by kind, board and port; identical radios stay apart; the same walk twice is no change
        auto node = [](uint16_t vid, uint16_t pid, const char* path) { UsbNode n; n.vid = vid; n.pid = pid; n.path = path; return n; };
        const auto a = usbRadioDevices({node(0x0bda, 0x2838, "1-2"), node(0x0bda, 0x2838, "1-1"), node(0x046d, 0xc077, "1-5"), node(0x1d50, 0x6089, "1-3")});
        CHECK(a.size() == 3 && a[0].usbPath == "1-1" && a[2].kind == DeviceInfo::HackRF && a[0].kind == DeviceInfo::Native, "walk devices: %zu", a.size());
        CHECK(radioKey(a[0]) != radioKey(a[1]), "two identical dongles have two keys");
        CHECK(diffRadiosSettled(a, a).added.empty() && diffRadiosSettled(a, a).removed.empty(), "same walk: no change");
        RadioFind f;   // Find on walk devices: unplug and replug in the same port is the same radio; another port is flagged
        f.start(a, 0);
        const auto b = usbRadioDevices({node(0x0bda, 0x2838, "1-2"), node(0x1d50, 0x6089, "1-3")});
        f.feed(b, 1);
        CHECK(f.state() == RadioFind::WaitReplug && f.left().usbPath == "1-1", "unplug seen on the walk");
        f.feed(a, 2);
        CHECK(f.state() == RadioFind::Found && !f.wrongRadio() && f.returned().usbPath == "1-1", "same port: right radio");
        RadioFind g;
        g.start(a, 0); g.feed(b, 1);
        g.feed(usbRadioDevices({node(0x0bda, 0x2838, "1-2"), node(0x1d50, 0x6089, "1-3"), node(0x0bda, 0x2838, "1-4")}), 2);
        CHECK(g.state() == RadioFind::Found && g.wrongRadio(), "another port: flagged");
    }
    {   // matching a walk radio to a listed entry: by port, else the one-of-its-kind rule, else ambiguous
        DeviceInfo fobos; fobos.kind = DeviceInfo::Native; fobos.board = "fobos"; fobos.name = "Fobos SDR (agile)"; fobos.serial = "F1";
        DeviceInfo rtlA = rtl("A", "1-1"), plu; plu.kind = DeviceInfo::Native; plu.board = "sdrplay"; plu.name = "SDRplay RSP1A"; plu.serial = "S1";
        DeviceInfo sound; sound.kind = DeviceInfo::Audio; sound.name = "Sound card";
        const L list = {sound, rtlA, fobos, plu};
        auto walkDev = [](const char* board, const char* path) { DeviceInfo d; d.kind = DeviceInfo::Native; d.board = board; d.name = board; d.usbPath = path; return d; };
        auto m = matchWalkRadios(list, {walkDev("RTL-SDR", "1-1"), walkDev("Fobos SDR", "1-3"), walkDev("SDRplay RSP", "1-4")});
        CHECK(m[0].how == WalkMatch::ByPath && m[0].index == 1, "by port");
        CHECK(m[1].how == WalkMatch::ByKind && m[1].index == 2, "one Fobos, one Fobos entry without a port: %d", (int)m[1].how);
        CHECK(m[2].how == WalkMatch::ByKind && m[2].index == 3, "one SDRplay likewise");
        auto two = matchWalkRadios(list, {walkDev("Fobos SDR", "1-3"), walkDev("Fobos SDR", "1-5")});
        CHECK(two[0].how == WalkMatch::Ambiguous && two[1].how == WalkMatch::Ambiguous, "two Fobos on the bus, one entry: ambiguous");
        DeviceInfo fobos2 = fobos; fobos2.serial = "F2";
        auto two2 = matchWalkRadios({fobos, fobos2}, {walkDev("Fobos SDR", "1-3")});
        CHECK(two2[0].how == WalkMatch::Ambiguous, "one on the bus, two entries without ports: ambiguous");
        auto none = matchWalkRadios(list, {walkDev("HackRF", "1-9")});
        CHECK(none[0].how == WalkMatch::None && none[0].index == -1, "no HackRF entry: none");
        auto path = matchWalkRadios({rtl("A", "1-1"), fobos}, {walkDev("RTL-SDR", "1-2")});
        CHECK(path[0].how == WalkMatch::None, "an RTL on another port does not take the Fobos entry");
    }
    {   // a claim blocks the background listing
        CHECK(backgroundListingAllowed(false, false) && !backgroundListingAllowed(true, false) && !backgroundListingAllowed(false, true), "decision");
        DeviceInfo d; d.kind = DeviceInfo::Native; d.board = "rtlsdr"; d.serial = "CLAIMTEST"; d.nativeArgs = "0";
        CHECK(!anyDeviceClaimed(), "nothing claimed");
        CHECK(claimDevice(d, 7), "claim");
        CHECK(anyDeviceClaimed() && !beginBackgroundListing(), "claimed: the listing may not begin");
        releaseDevice(7);
        CHECK(!anyDeviceClaimed() && beginBackgroundListing(), "free: it may begin");
        CHECK(!beginBackgroundListing(), "one listing at a time");
        { BackgroundListingScope sc; CHECK(!sc.ok, "the scope does not begin a second listing"); }
        CHECK(!beginBackgroundListing(), "and does not end the first");
        std::atomic<bool> got{false};
        std::thread t([&] { got = claimDevice(d, 8); });   // a Start during the listing waits for it
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(!got, "a claim waits while the listing runs");
        endBackgroundListing();
        t.join();
        CHECK(got && anyDeviceClaimed(), "and goes through when it ends");
        releaseDevice(8);
        { BackgroundListingScope sc; CHECK(sc.ok, "scope begins"); }
        CHECK(beginBackgroundListing(), "the scope ended the listing"); endBackgroundListing();
        holdDeviceShared(d, 5);
        CHECK(anyDeviceClaimed() && !beginBackgroundListing(), "a shared hold (the transmitter on a Pluto) counts");
        releaseDeviceShared(5);
        CHECK(!anyDeviceClaimed(), "released");
    }
    return testSummary("radio_watch");
}
