// Receive-only radio tools (Tools > Radio tools): alarms that read what the Wi-Fi and Bluetooth LE modes already hear, a log of the
// networks and devices passed on a drive with a position (written to a file on this computer, never uploaded), and a carrier finder for
// the spectrum on screen. Nothing here transmits, joins a network, pairs with a device or captures a key exchange.
#pragma once
#include "dect2/wifi/wifi_tel.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace tools {

// ---- Evil twin check -------------------------------------------------------------------------------------------------------------------
// Access points that announce the same network name. That is normal (a mesh, several floors, a hotel); it is worth a look when the same
// name is open on one address and protected on another, because that is how a fake copy of a network is set up to catch a phone that
// joins by name. Protected-only differences (WPA2 and WPA3 mixes) are normal upgrades and are not flagged.
struct TwinGroup {
    std::string ssid;
    std::vector<size_t> aps;      // indices into the list that was checked
    int severity = 0;             // 1 note (different manufacturers), 2 check (open and protected under one name)
    std::string why;
};
std::vector<TwinGroup> findTwins(const std::vector<WifiAp>& aps);

// ---- Bluetooth serial module names -----------------------------------------------------------------------------------------------------
// The factory names of cheap serial Bluetooth modules (HC-05, HC-06, HC-08, JDY-xx, BT05 ...). Some card skimmers use one, so phone apps
// that look for skimmers list them; so do many hobby projects, so a match is a reason to look, not a finding.
bool skimmerName(const std::string& name, std::string* why = nullptr);

// ---- Deauthentication flood ------------------------------------------------------------------------------------------------------------
struct DeauthAlarm {
    bool active = false;
    int frames = 0;               // deauthentication and disassociation frames in the window
    std::string bssid;            // the network most of them name ("" when none)
    int bssidFrames = 0;
    std::string target;           // the address most of them go to (ff:ff:ff:ff:ff:ff: everyone)
    bool broadcast = false;
};
// Counts the frames of the last windowSec seconds before nowWall; active from threshold frames on.
DeauthAlarm deauthAlarm(const std::vector<WifiDeauth>& log, int64_t nowWall, int windowSec = 10, int threshold = 20);

// ---- Drive log -------------------------------------------------------------------------------------------------------------------------
struct LogEntry {
    std::string id;               // BSSID or Bluetooth address
    bool ble = false;
    std::string name;             // SSID or device name
    int security = 0;             // wifi::Security
    bool enterprise = false;
    int channel = 0;              // Wi-Fi channel, 0 for Bluetooth
    float rssi = -200;            // the strongest level heard (dBFS, as the radio reports it)
    double lat = 0, lon = 0, alt = 0;   // where that was
    int64_t firstWall = 0;
};
class DriveLog {
public:
    void setBle(bool on) { ble_ = on; }
    bool ble() const { return ble_; }
    // A network or device heard at a position. A known id keeps its strongest sighting.
    void addWifi(const WifiAp& ap, double lat, double lon, double alt, int64_t wall);
    void addBle(const std::string& addr, const std::string& name, float rssi, double lat, double lon, double alt, int64_t wall);
    size_t size() const { return e_.size(); }
    size_t wifiCount() const;
    size_t bleCount() const;
    void clear() { e_.clear(); }
    std::vector<LogEntry> entries() const;
    // WiGLE CSV 1.4 (the plain file format; written to disk by the caller, uploading is up to the user). rssiOffsetDb turns the radio's relative level into dBm.
    std::string wigleCsv(double rssiOffsetDb = 0) const;
    std::string kml() const;
    static constexpr size_t kMax = 100000;
private:
    std::map<std::string, LogEntry> e_;
    bool ble_ = false;
};
std::string wigleAuth(int security, bool enterprise);   // "[WPA2-PSK-CCMP][ESS]"
std::string csvField(const std::string& s);
std::string xmlEscape(const std::string& s);
std::string utcStamp(int64_t wall);                      // "2026-10-24 00:14:05"

// ---- Carrier finder --------------------------------------------------------------------------------------------------------------------
struct Carrier {
    double hz = 0;                // absolute: the peak of a narrow signal, the middle of a wide one
    double levelDb = 0;           // the peak, dBFS
    double overFloorDb = 0;       // the peak above the median of the trace
    double widthHz = 0;           // the occupied width: from the first to the last bin above the threshold
};
// The signals of a spectrum line (dBFS per bin, DC-centred, as the main plot has it): every run of bins above the median plus overFloorDb is one
// signal, runs closer than mergeGapHz (0: 25 kHz) are joined so that one wideband signal is not listed as a dozen lines. The strongest topN, strongest first.
// centreHz and the sample rate place the bins.
std::vector<Carrier> findCarriers(const std::vector<float>& dbfs, double centreHz, double sampleRate, int topN = 12, double overFloorDb = 10.0, double mergeGapHz = 0);
double medianDb(const std::vector<float>& v);

struct BandPreset { const char* name; double mhz; };
const std::vector<BandPreset>& subGhzPresets();     // the licence-free sub-GHz bands used for remotes, sensors and meters

} // namespace tools
} // namespace dect2
