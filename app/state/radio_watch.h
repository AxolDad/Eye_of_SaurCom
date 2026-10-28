// Watching for radios being plugged in or out: the background poll, the "New radio" notice and "Find this radio" (unplug it and plug it back).
// The decisions are in core (dect2/source/radio_watch.h); this file polls, applies the new list to the receivers and talks to the user.
//
// How it polls (cheap, and it never opens a radio that is streaming):
//  1. Every 2 s (every 1 s while a Find is running), on a worker thread, one USB walk (usbEnumerateFresh: libusb descriptors, nothing opened).
//  2. Only when the set of known radio ids on the bus changed does it list the radios (listRadios, as Rescan does), on the same worker thread,
//     with the radios that are streaming held out of it (RadioListingKnown: a streaming HackRF is not opened again).
//  3. Never while the radio dropdown is open or the EEPROM tool is busy. One poll in flight at a time.
#pragma once
#include "dect2/source/radio_watch.h"
#include <string>

struct App;

void radioWatchTick(App& first);          // once per frame, from receiversFrame (not from a draw function)
void radioWatchDropdownOpen();            // the toolbar calls this every frame while its radio dropdown is open: no polling then
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
