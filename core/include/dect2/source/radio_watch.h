// Watching the radios that are plugged in: "Find this radio" (unplug it, plug it back) and the notice for a radio that was not there before.
// Pure logic fed with radio lists and a clock (seconds); no device is opened here. The app polls (app/state/radio_watch.cpp) and feeds these.
#pragma once
#include "dect2/source/radio_id.h"
#include "dect2/source/usb_path.h"
#include <set>
#include <string>
#include <vector>

namespace dect2 {

// The entries of a source list that a plugged-in radio watch cares about: radios found on this computer. Network radios and Hamlib rigs are
// something the user added by hand, not something plugged in, so they are left out.
std::vector<DeviceInfo> watchableRadios(const std::vector<DeviceInfo>& devs);

// diffRadios() with one more rule: a removed and an added radio of the same kind, board, serial and USB path are the same radio whose key
// changed (a second radio with its serial arrived or left, so "sn:" became "usb:" or back), not a radio that left or came.
RadioDiff diffRadiosSettled(const std::vector<DeviceInfo>& before, const std::vector<DeviceInfo>& after);

// A text that changes when a USB radio (by the table of known radio ids) is added to or removed from the bus or moves to another port.
// Built from a USB walk (usbEnumerate), which opens nothing; the app compares two of them before it lists the radios at all.
std::string radioUsbSignature(const std::vector<UsbNode>& nodes);

// One DeviceInfo per known radio on the USB bus, made from the descriptor walk alone (nothing opened): the kind from the id (HackRF, else Native),
// board and name from the id table, the USB path. No serial: its key is "<kind>:<board>:usb:<path>". Find and the new-radio notice are fed with these,
// so both sides of every comparison come from one source and a streaming radio is never touched.
std::vector<DeviceInfo> usbRadioDevices(const std::vector<UsbNode>& nodes);

// Which entry of a radio list is the radio the USB walk saw? By USB port when the entry has one. Kinds whose listing cannot say (SDRplay, a Fobos
// among several) have an empty port: then, when exactly one walk radio of that kind has no entry by port and exactly one listed entry of that kind
// has no port, they are the same radio. Anything else with candidates is Ambiguous (the user picks it in the list); no candidate is None.
struct WalkMatch {
    enum How { None, ByPath, ByKind, Ambiguous } how = None;
    int index = -1;   // into the list passed in, for ByPath and ByKind
};
std::vector<WalkMatch> matchWalkRadios(const std::vector<DeviceInfo>& list, const std::vector<DeviceInfo>& walk);

// The match for one walk radio `dev`, judged with the whole walk (so two Fobos on the bus are Ambiguous for each, and one that has left is judged with
// the ones still there). This is the call shape the app uses.
WalkMatch matchWalkRadio(const std::vector<DeviceInfo>& list, const std::vector<DeviceInfo>& walk, const DeviceInfo& dev);

// May the background watch run the radio listing (which opens radios)? Not while a radio is claimed (a receiver or the transmitter has it) and not
// while the EEPROM tool is working on a dongle.
inline bool backgroundListingAllowed(bool anyClaimed, bool eepromBusy) { return !anyClaimed && !eepromBusy; }

// "Find this radio": the user unplugs the radio they want to identify and plugs it back in; the radio that went away and came back is it.
class RadioFind {
public:
    enum State { Idle, WaitUnplug, WaitReplug, Found, TimedOut, Cancelled };
    static constexpr double kTimeoutS = 60;   // from start()

    // current: the radios as listed now. now: seconds on any steady clock.
    void start(const std::vector<DeviceInfo>& current, double now);
    // A new listing. WaitUnplug: waits until exactly one radio has left (several leaving at once: waits on; a radio that appears is ignored).
    // WaitReplug: the radio that came back is Found, with wrongRadio() set when its key differs from the one that left.
    void feed(const std::vector<DeviceInfo>& current, double now);
    // Time passes without a listing (the timeout).
    void tick(double now);
    void cancel();

    State state() const { return state_; }
    bool active() const { return state_ == WaitUnplug || state_ == WaitReplug; }
    const DeviceInfo& left() const { return left_; }          // valid from WaitReplug on
    const DeviceInfo& returned() const { return back_; }      // valid in Found
    bool wrongRadio() const { return wrong_; }
    double remaining(double now) const;                       // seconds until the timeout

private:
    State state_ = Idle;
    double t0_ = 0;
    std::vector<DeviceInfo> base_;   // WaitUnplug: the list at start; WaitReplug: the list when the radio had left
    DeviceInfo left_, back_;
    std::string leftKey_;
    bool wrong_ = false;
};

// Radios that appear while the app runs. Nothing in the first kGraceS seconds after start() (what was plugged in at launch is not news).
class NewRadioDetector {
public:
    static constexpr double kGraceS = 5;
    void start(const std::vector<DeviceInfo>& current, double now);
    // The radios to tell the user about: added since the last listing, not the same radio under another key, not seen earlier this session.
    std::vector<DeviceInfo> feed(const std::vector<DeviceInfo>& current, double now);
    bool seen(const std::string& key) const { return seen_.count(key) != 0; }
private:
    double t0_ = 0;
    bool started_ = false;
    std::vector<DeviceInfo> prev_;
    std::set<std::string> seen_;
};

}  // namespace dect2
