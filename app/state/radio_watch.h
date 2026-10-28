// Watching for radios being plugged in or out: the background poll, the "New radio" notice and "Find this radio" (unplug it and plug it back).
// The decisions are in core (dect2/source/radio_watch.h); this file polls, applies the new list to the receivers and talks to the user.
//
// How it polls (cheap, and it never opens a radio that is claimed):
//  1. Every 2 s (every 1 s while a Find runs), on a worker thread, one USB walk (usbEnumerateFresh: libusb descriptors, nothing opened).
//     Find and the New radio notice work on DeviceInfos made from that walk (kind from the USB id, the port), keyed by port.
//  2. When the set of radios on the bus changed, the radios are listed (listRadios, as Rescan does, so the dropdown gets serial and name),
//     but only while no radio is claimed by a receiver, a scan or the transmitter (core/src/engine/device_claims.cpp), re-checked atomically
//     in the worker; a Start waits in claimDevice() while such a listing runs. With radios claimed, the listing waits until they are free,
//     and Find/notices say where the radio is (USB port) and to press Rescan.
//  3. Never while the radio dropdown is open or the EEPROM tool is busy. One poll in flight at a time; joined at shutdown.
#pragma once
#include "dect2/source/radio_watch.h"
#include <string>

struct App;

void radioWatchTick(App& first);          // once per frame, from receiversFrame (not from a draw function)
void radioWatchDropdownOpen();            // the toolbar calls this every frame while its radio dropdown is open: no polling then
void radioWatchShutdown();                // wait for the worker; call before the radio libraries are torn down
void radioWatchEepromBusy(bool busy);     // Task 5: the EEPROM tool is reading or writing a dongle; no listing meanwhile

// Find this radio
void radioFindStart(App& a);              // opens the window and starts waiting for an unplug
const dect2::RadioFind& radioFind();
int radioFindRx();                        // the receiver whose toolbar started it
struct RadioFindView {
    std::string left;       // what the unplugged radio was called
    std::string back;       // what came back, as the radio list shows it now
    std::string note;       // why it could not be selected, if so
    bool selected = false;  // it was chosen in that receiver's radio list
    int devIdx = -1;        // its entry in that receiver's list, -1 when none
};
const RadioFindView& radioFindView();
bool radioFindOpenRequested(int rxId);    // true once for the toolbar of that receiver: open the window
void radioFindCancel();
double radioWatchNow();                   // seconds on the clock the watch uses

// The brief colour flash on the radio the Find found, in the radio dropdown (about 3 s).
bool radioFlashing(const std::string& key);
