#include "dect2/engine/device_claims.h"
#include <condition_variable>
#include <map>
#include <mutex>

namespace dect2 {
namespace {
std::mutex gMu;
std::map<std::string, int> gHeld;
std::multimap<int, std::string> gShared;   // owner -> radio key: a transmitter that shares a full-duplex radio
std::condition_variable gCv;
bool gListing = false;   // a background listing is running (guarded by gMu)
}

std::string deviceClaimKey(const DeviceInfo& d) {
    if (!d.isRadio() && !d.isAudio()) return "";
    std::string id = !d.serial.empty() ? d.serial : !d.nativeArgs.empty() ? d.nativeArgs : !d.soapyArgs.empty() ? d.soapyArgs : d.name;
    return std::to_string((int)d.kind) + "|" + d.board + "|" + id;
}

bool claimDevice(const DeviceInfo& d, int owner, int* holder) {
    const std::string k = deviceClaimKey(d);
    if (k.empty()) return true;
    std::unique_lock<std::mutex> lk(gMu);
    gCv.wait(lk, [] { return !gListing; });   // not while the radios are being listed
    auto it = gHeld.find(k);
    if (it != gHeld.end() && it->second != owner) { if (holder) *holder = it->second; return false; }
    gHeld[k] = owner;
    return true;
}

void releaseDevice(int owner) {
    std::lock_guard<std::mutex> lk(gMu);
    for (auto it = gHeld.begin(); it != gHeld.end();) it = it->second == owner ? gHeld.erase(it) : std::next(it);
}

bool anyDeviceClaimed() {
    std::lock_guard<std::mutex> lk(gMu);
    return !gHeld.empty() || !gShared.empty();
}

void holdDeviceShared(const DeviceInfo& d, int owner) {
    const std::string k = deviceClaimKey(d);
    if (k.empty()) return;
    std::unique_lock<std::mutex> lk(gMu);
    gCv.wait(lk, [] { return !gListing; });   // not while the radios are being listed
    gShared.emplace(owner, k);
}

void releaseDeviceShared(int owner) {
    std::lock_guard<std::mutex> lk(gMu);
    gShared.erase(owner);
}

bool beginBackgroundListing() {
    std::lock_guard<std::mutex> lk(gMu);
    if (!gHeld.empty() || !gShared.empty() || gListing) return false;
    gListing = true;
    return true;
}

void endBackgroundListing() {
    { std::lock_guard<std::mutex> lk(gMu); gListing = false; }
    gCv.notify_all();
}

std::string deviceHolderText(int holder) {
    return holder >= kHubClaimOwner ? "the direction-finding hub" : holder >= kSweepClaimBase ? "a sweep tool" : holder >= kTxClaimOwner ? "the transmitter" : "RX " + std::to_string(holder);
}

int deviceHolder(const DeviceInfo& d) {
    const std::string k = deviceClaimKey(d);
    if (k.empty()) return 0;
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gHeld.find(k);
    return it == gHeld.end() ? 0 : it->second;
}

}
