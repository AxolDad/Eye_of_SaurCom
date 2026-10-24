// Receive-only radio tools, see radio_tools.h
#include "dect2/tools/radio_tools.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>

namespace dect2 {
namespace tools {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
// open or WEP: nothing that keeps an eavesdropper out; OWE encrypts without a password, so it is its own class and not compared
int secClass(int s) {
    if (s == wifi::Open) return 0;
    if (s == wifi::Wep) return 1;
    if (s == wifi::Owe) return 3;
    return 2;
}
} // namespace

std::vector<TwinGroup> findTwins(const std::vector<WifiAp>& aps) {
    std::map<std::string, std::vector<size_t>> by;
    for (size_t i = 0; i < aps.size(); i++)
        if (!aps[i].hidden && !aps[i].ssid.empty()) by[aps[i].ssid].push_back(i);
    std::vector<TwinGroup> out;
    for (auto& kv : by) {
        if (kv.second.size() < 2) continue;
        bool open = false, wep = false, prot = false;
        std::set<std::string> vendors;
        bool anyLocal = false;
        for (size_t i : kv.second) {
            const int c = secClass(aps[i].security);
            open |= c == 0; wep |= c == 1; prot |= c == 2;
            if (aps[i].vendor == "local") anyLocal = true;
            else if (!aps[i].vendor.empty()) vendors.insert(aps[i].vendor);
        }
        TwinGroup g;
        g.ssid = kv.first;
        g.aps = kv.second;
        if ((open || wep) && prot) {
            g.severity = 2;
            g.why = std::string("the same name is ") + (open ? "open" : "WEP") + " on one address and protected on another";
        } else if (vendors.size() >= 2 && !anyLocal) {
            g.severity = 1;
            g.why = "the same name comes from access points of different manufacturers (usual in a shared or hotel network)";
        } else continue;
        out.push_back(std::move(g));
    }
    std::sort(out.begin(), out.end(), [](const TwinGroup& a, const TwinGroup& b) { return a.severity > b.severity; });
    return out;
}

bool skimmerName(const std::string& name, std::string* why) {
    const std::string n = lower(name);
    static const char* const kPrefix[] = {"hc-05", "hc-06", "hc-08", "hc05", "hc06", "hc08", "bt-hc05", "bt-hc06", "linvor", "jdy-", "bt04", "bt05", "mlt-bt05", "rnbt", "spp-ca", "hm-10", "hm10"};
    for (const char* p : kPrefix) {
        if (n.rfind(p, 0) == 0) {
            if (why) *why = "the factory name of a cheap serial Bluetooth module (" + name + "); some card skimmers use one, and so do many hobby projects";
            return true;
        }
    }
    return false;
}

DeauthAlarm deauthAlarm(const std::vector<WifiDeauth>& log, int64_t nowWall, int windowSec, int threshold) {
    DeauthAlarm a;
    std::map<std::string, int> bss, dst;
    for (const WifiDeauth& d : log) {
        if (nowWall - d.wall > windowSec || d.wall > nowWall + 2) continue;
        a.frames++;
        bss[d.bssid]++;
        dst[d.dest]++;
    }
    a.active = a.frames >= threshold;
    for (auto& kv : bss) if (kv.second > a.bssidFrames) { a.bssidFrames = kv.second; a.bssid = kv.first; }
    int best = 0;
    for (auto& kv : dst) if (kv.second > best) { best = kv.second; a.target = kv.first; }
    a.broadcast = a.target == "FF:FF:FF:FF:FF:FF" || a.target == "ff:ff:ff:ff:ff:ff";
    return a;
}

std::string wigleAuth(int s, bool ent) {
    const char* k = ent ? "EAP" : "PSK";
    char b[160];
    switch (s) {
    case wifi::Open: return "[ESS]";
    case wifi::Wep: return "[WEP][ESS]";
    case wifi::Wpa: snprintf(b, sizeof b, "[WPA-%s-TKIP][ESS]", k); return b;
    case wifi::WpaWpa2: snprintf(b, sizeof b, "[WPA-%s-TKIP+CCMP][WPA2-%s-CCMP][ESS]", k, k); return b;
    case wifi::Wpa2: snprintf(b, sizeof b, "[WPA2-%s-CCMP][ESS]", k); return b;
    case wifi::Wpa2Wpa3: snprintf(b, sizeof b, "[WPA2-%s-CCMP][WPA3-SAE-CCMP][ESS]", k); return b;
    case wifi::Wpa3: return "[WPA3-SAE-CCMP][ESS]";
    case wifi::Owe: return "[OWE][ESS]";
    default: return "[ESS]";
    }
}

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; if (c != '\r' && c != '\n') o += c; }
    return o + "\"";
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: if ((unsigned char)c >= 32 || c == '\t') o += c;
        }
    }
    return o;
}

std::string utcStamp(int64_t wall) {
    const std::time_t t = (std::time_t)wall;
    std::tm g = *std::gmtime(&t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &g);
    return b;
}

void DriveLog::addWifi(const WifiAp& ap, double lat, double lon, double alt, int64_t wall) {
    if (ap.bssid.empty()) return;
    auto it = e_.find(ap.bssid);
    if (it == e_.end()) {
        if (e_.size() >= kMax) return;
        LogEntry e;
        e.id = ap.bssid; e.firstWall = wall; e.rssi = -1000;
        it = e_.emplace(ap.bssid, std::move(e)).first;
    }
    LogEntry& e = it->second;
    if (!ap.ssid.empty()) e.name = ap.ssid;
    e.security = ap.security; e.enterprise = ap.enterprise;
    if (ap.channel) e.channel = ap.channel;
    if (ap.rssi > e.rssi) { e.rssi = ap.rssi; e.lat = lat; e.lon = lon; e.alt = alt; }
}

void DriveLog::addBle(const std::string& addr, const std::string& name, float rssi, double lat, double lon, double alt, int64_t wall) {
    if (!ble_ || addr.empty()) return;
    auto it = e_.find(addr);
    if (it == e_.end()) {
        if (e_.size() >= kMax) return;
        LogEntry e;
        e.id = addr; e.ble = true; e.firstWall = wall; e.rssi = -1000;
        it = e_.emplace(addr, std::move(e)).first;
    }
    LogEntry& e = it->second;
    if (!name.empty()) e.name = name;
    if (rssi > e.rssi) { e.rssi = rssi; e.lat = lat; e.lon = lon; e.alt = alt; }
}

size_t DriveLog::wifiCount() const { size_t n = 0; for (auto& kv : e_) if (!kv.second.ble) n++; return n; }
size_t DriveLog::bleCount() const { return e_.size() - wifiCount(); }

std::vector<LogEntry> DriveLog::entries() const {
    std::vector<LogEntry> v;
    for (auto& kv : e_) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), [](const LogEntry& a, const LogEntry& b) { return a.firstWall < b.firstWall || (a.firstWall == b.firstWall && a.id < b.id); });
    return v;
}

std::string DriveLog::wigleCsv(double off) const {
    std::string o = "WigleWifi-1.4,appRelease=0.2.9,model=EyeOfZorCom,release=0.2.9,device=EyeOfZorCom,display=,board=,brand=EyeOfZorCom\n";
    o += "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n";
    char b[160];
    for (const LogEntry& e : entries()) {
        o += e.id + "," + csvField(e.name) + "," + (e.ble ? std::string("Misc") : wigleAuth(e.security, e.enterprise)) + "," + utcStamp(e.firstWall) + ",";
        snprintf(b, sizeof b, "%d,%d,%.7f,%.7f,%.1f,0,%s\n", e.channel, (int)std::lround(e.rssi + off), e.lat, e.lon, e.alt, e.ble ? "BLE" : "WIFI");
        o += b;
    }
    return o;
}

std::string DriveLog::kml() const {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><name>Eye of ZorCom drive log</name>\n";
    char b[160];
    for (const LogEntry& e : entries()) {
        o += "<Placemark><name>" + xmlEscape(e.name.empty() ? e.id : e.name) + "</name><description>" + xmlEscape(e.id) +
             (e.ble ? " Bluetooth LE" : " Wi-Fi " + wigleAuth(e.security, e.enterprise)) + "</description>";
        snprintf(b, sizeof b, "<Point><coordinates>%.7f,%.7f,%.1f</coordinates></Point></Placemark>\n", e.lon, e.lat, e.alt);
        o += b;
    }
    return o + "</Document></kml>\n";
}

double medianDb(const std::vector<float>& v) {
    if (v.empty()) return -200;
    std::vector<float> c = v;
    std::nth_element(c.begin(), c.begin() + (ptrdiff_t)c.size() / 2, c.end());
    return c[c.size() / 2];
}

std::vector<Carrier> findCarriers(const std::vector<float>& d, double centreHz, double fs, int topN, double over, double mergeGapHz) {
    std::vector<Carrier> out;
    if (d.size() < 8 || fs <= 0) return out;
    const int n = (int)d.size();
    const double binHz = fs / (double)n;
    const double floorDb = medianDb(d);
    const double thr = floorDb + over;
    const int gap = std::max(2, (int)std::lround((mergeGapHz > 0 ? mergeGapHz : 25e3) / binHz));
    const double f0 = centreHz - fs / 2;
    int i = 0;
    while (i < n) {
        if (d[(size_t)i] < thr) { i++; continue; }
        int first = i, last = i, k = i + 1;
        for (; k < n && k - last <= gap; k++) if (d[(size_t)k] >= thr) last = k;   // a short dip below the threshold does not end the signal
        int peak = first;
        for (int m = first; m <= last; m++) if (d[(size_t)m] > d[(size_t)peak]) peak = m;
        Carrier c;
        c.levelDb = d[(size_t)peak];
        c.overFloorDb = c.levelDb - floorDb;
        c.widthHz = (double)(last - first + 1) * binHz;
        if (last - first + 1 <= 3 && peak > 0 && peak + 1 < n) {      // a narrow line: interpolate its peak between the bins
            const double a = d[(size_t)peak - 1], b = d[(size_t)peak], cc = d[(size_t)peak + 1], den = a - 2 * b + cc;
            const double off = std::fabs(den) > 1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (a - cc) / den)) : 0;
            c.hz = f0 + ((double)peak + off) * binHz;
        } else {
            c.hz = f0 + 0.5 * (double)(first + last) * binHz;
        }
        out.push_back(c);
        i = last + 1;
    }
    std::sort(out.begin(), out.end(), [](const Carrier& a, const Carrier& b) { return a.levelDb > b.levelDb; });
    if ((int)out.size() > topN) out.resize((size_t)std::max(0, topN));
    return out;
}

const std::vector<BandPreset>& subGhzPresets() {
    static const std::vector<BandPreset> v = {
        {"315 MHz (North America remotes)", 315.0}, {"390 MHz", 390.0}, {"433.92 MHz (ISM, worldwide)", 433.92},
        {"868.35 MHz (Europe SRD)", 868.35}, {"915 MHz (Americas, Australia ISM)", 915.0}};
    return v;
}

} // namespace tools
} // namespace dect2
