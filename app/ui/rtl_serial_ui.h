// "Give this RTL-SDR a unique serial…" and "Restore from backup…": the confirmation windows for rewriting a dongle's EEPROM
// (core: dect2/source/rtl_serial.h). The write runs on a worker thread; the radio watch does not list radios meanwhile.
#pragma once
struct App;

// Buttons for the radio settings of the selected radio: the two actions when it is an RTL2832U (nothing otherwise). Disabled while the radio is in use.
void rtlSerialButtons(App& a);
// The shared-serial warning's button: opens the tool for the RTL-SDR that shares its serial (the selected one when it does). Nothing when there is none.
void rtlSerialWarningButton(App& a, bool small);
// Open the window for this entry of a.devices.
void rtlSerialRequest(App& a, int deviceIdx);
void rtlSerialRestoreRequest(App& a, int deviceIdx);
// Draw the windows; call every frame next to radioNameModal.
void rtlSerialModal(App& a);
// Wait for a running write before the radio libraries are torn down.
void rtlSerialShutdown();
// True while a write or restore runs: Rescan is off then.
bool rtlSerialBusy();
// Once per frame from the app: takes the result of a finished write whichever receiver is current, clears the busy state and says the result as a notice.
void rtlSerialTick();
