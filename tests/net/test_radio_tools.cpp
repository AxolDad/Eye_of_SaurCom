// Receive-only radio tools (core/src/tools/radio_tools.cpp): the evil twin check, Bluetooth module names, the deauthentication flood alarm,
// the drive log with its WiGLE CSV and KML, and the carrier finder.
#include "dect2/tools/radio_tools.h"
#include "dect2/wifi/wifi_proto.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::tools;
#include "../common/test_util.h"

static WifiAp ap(const char* bssid, const char* ssid, int sec, const char* vendor = "Acme", int ch = 6, float rssi = -50) {
    WifiAp a;
    a.bssid = bssid; a.ssid = ssid; a.security = sec; a.vendor = vendor; a.channel = ch; a.rssi = rssi;
    return a;
}

int main() {
    // evil twin check
    {
        std::vector<WifiAp> v = {ap("00:11:22:33:44:01", "Cafe", wifi::Wpa2), ap("00:11:22:33:44:02", "Cafe", wifi::Open),   // open next to protected
                                 ap("00:11:22:33:55:01", "Home", wifi::Wpa2), ap("00:11:22:33:55:02", "Home", wifi::Wpa3),    // a normal mix
                                 ap("00:11:22:33:66:01", "Hotel", wifi::Wpa2, "Acme"), ap("00:11:22:33:66:02", "Hotel", wifi::Wpa2, "Other"),
                                 ap("00:11:22:33:77:01", "Alone", wifi::Open), ap("00:11:22:33:88:01", "Transition", wifi::Open), ap("00:11:22:33:88:02", "Transition", wifi::Owe)};
        WifiAp hidden = ap("00:11:22:33:99:01", "", wifi::Open); hidden.hidden = true;
        v.push_back(hidden); v.push_back(hidden);
        const auto g = findTwins(v);
        CHECK(g.size() == 2, "two groups: %zu", g.size());
        CHECK(g.size() == 2 && g[0].ssid == "Cafe" && g[0].severity == 2 && g[0].aps.size() == 2, "Cafe first, severity 2");
        CHECK(g.size() == 2 && g[1].ssid == "Hotel" && g[1].severity == 1, "Hotel second, a note");
        CHECK(g[0].why.find("open on one address and protected on another") != std::string::npos, "why: %s", g[0].why.c_str());
        // a WEP network next to a protected one counts, and a local (virtual) address hides the vendor note
        std::vector<WifiAp> w = {ap("00:11:22:33:44:01", "X", wifi::Wep), ap("00:11:22:33:44:02", "X", wifi::Wpa2)};
        CHECK(findTwins(w).size() == 1 && findTwins(w)[0].why.find("WEP") != std::string::npos, "WEP");
        std::vector<WifiAp> l = {ap("02:11:22:33:44:01", "Y", wifi::Wpa2, "local"), ap("00:11:22:33:44:02", "Y", wifi::Wpa2, "Acme"), ap("00:11:22:33:44:03", "Y", wifi::Wpa2, "Other")};
        CHECK(findTwins(l).empty(), "a virtual BSSID in the group: no vendor note");
        CHECK(findTwins({}).empty(), "empty");
    }
    // Bluetooth module names
    {
        std::string why;
        CHECK(skimmerName("HC-05", &why) && why.find("HC-05") != std::string::npos, "HC-05 %s", why.c_str());
        CHECK(skimmerName("hc-06") && skimmerName("JDY-31-SPP") && skimmerName("linvor") && skimmerName("BT05") && skimmerName("HM-10"), "other module names");
        CHECK(!skimmerName("") && !skimmerName("JBL Flip 5") && !skimmerName("Galaxy Buds") && !skimmerName("My HC-05"), "ordinary names");
    }
    // deauthentication flood
    {
        std::vector<WifiDeauth> log;
        for (int i = 0; i < 25; i++) { WifiDeauth d; d.wall = 1000 - i / 5; d.bssid = i % 5 ? "AA:AA:AA:AA:AA:01" : "AA:AA:AA:AA:AA:02"; d.dest = "FF:FF:FF:FF:FF:FF"; log.push_back(d); }
        DeauthAlarm a = deauthAlarm(log, 1000);
        CHECK(a.active && a.frames == 25 && a.bssid == "AA:AA:AA:AA:AA:01" && a.bssidFrames == 20 && a.broadcast, "flood: %d frames, %s %d", a.frames, a.bssid.c_str(), a.bssidFrames);
        a = deauthAlarm(log, 1000, 10, 30);
        CHECK(!a.active && a.frames == 25, "below a higher threshold");
        a = deauthAlarm(log, 1020);
        CHECK(!a.active && a.frames == 0, "old frames fall out of the window");
        WifiDeauth one; one.wall = 1000; one.bssid = "AA:AA:AA:AA:AA:03"; one.dest = "11:22:33:44:55:66";
        a = deauthAlarm({one}, 1000);
        CHECK(!a.active && a.frames == 1 && !a.broadcast && a.target == "11:22:33:44:55:66", "a single frame is not a flood");
    }
    // drive log
    {
        DriveLog L;
        WifiAp a1 = ap("00:11:22:33:44:01", "Cafe, \"Main\"", wifi::Wpa2, "Acme", 11, -70);
        L.addWifi(a1, 25.20, 55.40, 12, 1792800000);
        a1.rssi = -40;
        L.addWifi(a1, 25.21, 55.41, 13, 1792800010);      // stronger: its position wins
        a1.rssi = -90;
        L.addWifi(a1, 25.30, 55.50, 14, 1792800020);      // weaker: ignored
        L.addWifi(ap("00:11:22:33:44:02", "Open", wifi::Open, "Acme", 1, -60), 25.22, 55.42, 10, 1792800030);
        L.addBle("AA:BB:CC:DD:EE:FF", "Watch", -55, 25.2, 55.4, 0, 1792800040);   // Bluetooth is off by default
        CHECK(L.size() == 2 && L.wifiCount() == 2 && L.bleCount() == 0, "two networks, no Bluetooth: %zu", L.size());
        L.setBle(true);
        L.addBle("AA:BB:CC:DD:EE:FF", "Watch", -55, 25.2, 55.4, 0, 1792800040);
        CHECK(L.size() == 3 && L.bleCount() == 1, "Bluetooth when asked");
        const auto e = L.entries();
        CHECK(e.size() == 3 && e[0].lat == 25.21 && e[0].rssi == -40, "strongest sighting kept: %.2f %.0f", e[0].lat, e[0].rssi);
        const std::string csv = L.wigleCsv(-20);
        CHECK(csv.rfind("WigleWifi-1.4,appRelease=", 0) == 0, "WiGLE header");
        CHECK(csv.find("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n") != std::string::npos, "column line");
        CHECK(csv.find("00:11:22:33:44:01,\"Cafe, \"\"Main\"\"\",[WPA2-PSK-CCMP][ESS],") != std::string::npos, "quoted SSID and auth: %s", csv.c_str());
        CHECK(csv.find(",11,-60,25.2100000,55.4100000,13.0,0,WIFI\n") != std::string::npos, "level with the offset, position, type");
        CHECK(csv.find("AA:BB:CC:DD:EE:FF,Watch,Misc,") != std::string::npos && csv.find(",BLE\n") != std::string::npos, "Bluetooth row");
        CHECK(csv.find("2026-10-24 00:") != std::string::npos, "UTC stamps: %s", utcStamp(1792800000).c_str());
        const std::string kml = L.kml();
        CHECK(kml.find("<name>Cafe, &quot;Main&quot;</name>") != std::string::npos && kml.find("<coordinates>55.4100000,25.2100000,13.0</coordinates>") != std::string::npos, "KML");
        L.clear();
        CHECK(L.size() == 0, "clear");
        CHECK(wigleAuth(wifi::Wpa3, false) == "[WPA3-SAE-CCMP][ESS]" && wigleAuth(wifi::Wpa2, true) == "[WPA2-EAP-CCMP][ESS]" && wigleAuth(wifi::Open, false) == "[ESS]", "auth strings");
        CHECK(csvField("a,b") == "\"a,b\"" && csvField("plain") == "plain" && xmlEscape("<a&b>") == "&lt;a&amp;b&gt;", "escapes");
    }
    // carrier finder: a 2 Msps trace around 433.92 MHz with two lines
    {
        const size_t n = 4096;
        std::vector<float> d(n, -90.f);
        for (size_t i = 0; i < n; i++) d[i] += (float)((i * 7919) % 13) * 0.3f - 1.8f;   // a rough floor
        const double fs = 2e6, centre = 433.92e6, bin = fs / n;
        auto line = [&](double hz, float level) { const size_t i = (size_t)std::lround((hz - (centre - fs / 2)) / bin); d[i - 1] = level - 6; d[i] = level; d[i + 1] = level - 6; };
        line(433.92e6, -30.f);
        line(434.30e6, -55.f);
        const auto c = findCarriers(d, centre, fs, 5, 10);
        CHECK(c.size() == 2, "two carriers: %zu", c.size());
        CHECK(c.size() == 2 && std::fabs(c[0].hz - 433.92e6) < bin && c[0].levelDb > -31 && c[0].overFloorDb > 50 && c[0].widthHz <= 4 * bin, "the strong one: %.0f Hz %.1f dB", c[0].hz, c[0].levelDb);
        CHECK(c.size() == 2 && std::fabs(c[1].hz - 434.30e6) < bin && c[1].levelDb < c[0].levelDb, "the weaker one second");
        CHECK(findCarriers(d, centre, fs, 5, 80).empty(), "nothing 80 dB above the floor");
        // a wideband signal with short dips is one signal, not a dozen lines: 200 kHz wide around 433.2 MHz
        std::vector<float> w = d;
        const size_t lo = (size_t)std::lround((433.1e6 - (centre - fs / 2)) / bin), hi = (size_t)std::lround((433.3e6 - (centre - fs / 2)) / bin);
        for (size_t i = lo; i <= hi; i++) w[i] = -45.f + (float)((i * 31) % 7);
        for (size_t i = lo + 20; i < lo + 22; i++) w[i] = -90.f;      // two dips, 4 kHz each
        for (size_t i = lo + 60; i < lo + 63; i++) w[i] = -90.f;
        const auto cw = findCarriers(w, centre, fs, 5, 10);
        CHECK(cw.size() == 3, "the wide signal and the two lines: %zu", cw.size());
        const Carrier* wide = nullptr;
        for (const Carrier& x : cw) if (!wide || x.widthHz > wide->widthHz) wide = &x;
        CHECK(wide && std::fabs(wide->hz - 433.2e6) < 5e3 && std::fabs(wide->widthHz - 200e3) < 3 * bin, "one wide signal: %.0f Hz, width %.0f Hz", wide ? wide->hz : 0.0, wide ? wide->widthHz : 0.0);
        const auto narrow = findCarriers(w, centre, fs, 5, 10, 1e3);   // a 1 kHz merge gap: the dips split it
        CHECK(narrow.size() > cw.size(), "a short merge gap splits the dips: %zu", narrow.size());
        CHECK(findCarriers({}, centre, fs).empty() && findCarriers(d, centre, 0).empty(), "empty input");
        CHECK(std::fabs(medianDb(d) + 90.0) < 3, "median %.1f", medianDb(d));
        CHECK(subGhzPresets().size() >= 5 && subGhzPresets()[2].mhz == 433.92, "presets");
    }
    printf(fails ? "FAILED\n" : "radio_tools ok\n");
    return fails ? 1 : 0;
}
