// Which receiver (Engine) has which radio open, so one radio is never opened twice in a process (multi-SDR, docs/multi-sdr.md).
#pragma once
#include "dect2/source/source.h"
#include <string>

namespace dect2 {

// "" for the test signal and files (any number of receivers may use them); else a key that is the same for the same physical radio
std::string deviceClaimKey(const DeviceInfo& d);
// Claims the radio for receiver `owner` (1-based). False when another receiver has it: *holder is that receiver's number.
bool claimDevice(const DeviceInfo& d, int owner, int* holder = nullptr);
void releaseDevice(int owner);                          // everything the receiver holds
int deviceHolder(const DeviceInfo& d);                  // 0 = free, else the receiver number
constexpr int kSweepClaimBase = 1100;                   // sweep, station health and RFI tools: 1100 to 1199, one number per open tool
constexpr int kHubClaimOwner = 1200;
constexpr int kEepromClaimOwner = 1300;                // the RTL-SDR serial tool (core/.../rtl_serial.h) holds the dongle it writes under this number                    // the direction-finding hub's dongles (app/df)
constexpr int kTxClaimOwner = 1000;                     // the transmitter (tx.h) holds its radio under this number; receivers are 1 to 99
// The transmitter on a full-duplex radio (a PlutoSDR) shares the radio with a receiver: it holds it without excluding anyone, but the radio still counts
// as open for anyDeviceClaimed() (so the background watch does not list it).
void holdDeviceShared(const DeviceInfo& d, int owner);
void releaseDeviceShared(int owner);
bool anyDeviceClaimed();                                // some receiver or the transmitter has a radio (or sound card) open
// The background radio watch lists radios only while nothing is claimed (listing opens radios). begin... returns false when anything is claimed;
// otherwise it marks a listing as running, and claimDevice() waits (a few seconds at most) until endBackgroundListing(), so a radio is never
// opened for streaming while it is being listed. Always pair with end... when begin... returned true.
bool beginBackgroundListing();
void endBackgroundListing();
struct BackgroundListingScope {   // begin... and end... as a scope: end... runs even when the listing throws
    bool ok;
    BackgroundListingScope() : ok(beginBackgroundListing()) {}
    ~BackgroundListingScope() { if (ok) endBackgroundListing(); }
    BackgroundListingScope(const BackgroundListingScope&) = delete;
    BackgroundListingScope& operator=(const BackgroundListingScope&) = delete;
};
std::string deviceHolderText(int holder);               // "RX 2" or "the transmitter", for messages

}
