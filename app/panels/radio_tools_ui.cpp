// Tools > Radio tools: one window that gathers the receive-only tools of the program the way a pocket multi-tool groups them, and holds the
// four that read what the Wi-Fi, Bluetooth LE and spectrum views already have: alarms (deauthentication flood, evil twin check, Bluetooth
// serial module names), a frequency analyzer, and a drive log with a position written to a file on this computer. Nothing here transmits,
// joins a network, pairs with a device or captures a key exchange. core/src/tools/radio_tools.cpp has the logic.
#include "../app.h"
#include "../state/gps_serial.h"
#include "dect2/rfi/rfi.h"
#include "dect2/tools/radio_tools.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

namespace {

namespace rt = dect2::tools;

enum GpsMode { GpsReceiver, GpsNmea, NumGps };

struct State {
    bool loaded = false;
    int wantTab = -1;                  // a launcher asks for a tab
    // frequency analyzer
    int topN = 12;
    float overDb = 12.f;
    bool hold = false;
    std::vector<rt::Carrier> carriers;
    double carrierCentre = 0;
    // drive log
    rt::DriveLog log;
    bool logging = false;
    int gpsMode = GpsReceiver;
    char nmeaPath[256] = "";
    float rssiOffset = 0.f;
    bool includeBle = false;
    std::string status;
    double lastFeed = 0;
    SerialNmea gps;   // the GPS reader (app/state/gps_serial.cpp)
};
State S;

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.gpsMode = std::clamp((int)p.getI("rtGps", GpsReceiver), 0, NumGps - 1);
    snprintf(S.nmeaPath, sizeof S.nmeaPath, "%s", p.getS("rtNmea", "").c_str());
    S.rssiOffset = (float)p.getD("rtRssiOffset", 0.0);
    S.includeBle = p.getI("rtBle", 0) != 0;
    S.topN = std::clamp((int)p.getI("rtTopN", 12), 4, 30);
    S.overDb = (float)std::clamp(p.getD("rtOver", 12.0), 5.0, 40.0);
    S.log.setBle(S.includeBle);
}

void save() {
    plat::Prefs& p = plat::prefs();
    p.setI("rtGps", S.gpsMode); p.setS("rtNmea", S.nmeaPath); p.setD("rtRssiOffset", S.rssiOffset);
    p.setI("rtBle", S.includeBle ? 1 : 0); p.setI("rtTopN", S.topN); p.setD("rtOver", S.overDb);
}

// ---- GPS from an NMEA stream (a serial GPS puck or a log file), read on its own thread (app/state/gps_serial.cpp) ----
void stopGps() { S.gps.stop(); }
void startGps() {
    stopGps();
    if (S.gpsMode != GpsNmea || !S.nmeaPath[0]) return;
    S.gps.start(S.nmeaPath, 9600);
}

bool position(double& lat, double& lon, double& alt) {
    alt = 0;
    if (S.gpsMode == GpsReceiver) {
        if (!receiverLocationSet()) return false;
        receiverLocation(lat, lon);
        alt = receiverAltitudeM();
        return true;
    }
    dect2::geo::GpsFix f;
    if (!S.gps.latest(f)) return false;
    lat = f.lat; lon = f.lon;
    return true;
}

// what the Wi-Fi and Bluetooth views heard in the last seconds goes into the log with the position of now
void feedLog(App& a) {
    if (!S.logging) return;
    const double now = ImGui::GetTime();
    if (now - S.lastFeed < 1.0) return;
    S.lastFeed = now;
    double lat, lon, alt;
    if (!position(lat, lon, alt)) { S.status = S.gpsMode == GpsNmea ? "waiting for a position fix" : "set the receiver position first (Tools > Map or the setup)"; return; }
    S.status.clear();
    const int64_t wall = (int64_t)std::time(nullptr);
    for (const WifiAp& ap : a.rx.wifi.aps)
        if (wall - ap.lastWall <= 3) S.log.addWifi(ap, lat, lon, alt, wall);
    if (S.includeBle)
        for (const auto& d : a.rx.ble.devices)
            if (wall - d.lastWall <= 3) S.log.addBle(d.addr, d.name, d.rssi, lat, lon, alt, wall);
}

void tuneTo(App& a, double hz) {
    a.freqMhz = hz / 1e6;
    if (a.engine.running()) { a.tune.centerHz = hz; a.engine.retune(a.tune); }
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << text;
    return f.good();
}

void launcher(App& a, const char* name, const char* what, int family, bool* window, int tab) {
    ImGui::PushID(name);
    if (ImGui::SmallButton(tr("Open"))) {
        if (family) selectMode(a, family);
        if (window) *window = true;
        if (tab >= 0) S.wantTab = tab;
    }
    ImGui::PopID();
    ImGui::SameLine();
    ImGui::TextUnformatted(tr(name));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr(what));
}

void overview(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Receive only: nothing here transmits, joins a network, pairs with a device or captures a key exchange."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("Wi-Fi"));
    launcher(a, "Wi-Fi networks", "the access points around you, from their beacons", 54, nullptr, -1);
    launcher(a, "Wi-Fi alerts", "deauthentication flood alarm and evil twin check", 0, nullptr, 1);
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("Bluetooth"));
    launcher(a, "Bluetooth LE scanner", "devices and beacons that advertise", 51, nullptr, -1);
    launcher(a, "Tracker detector", "AirTag, Tile and other item trackers, and one that follows you", 143, nullptr, -1);
    launcher(a, "Serial module names", "HC-05 type names that some card skimmers use", 0, nullptr, 2);
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("Sub-GHz and spectrum"));
    launcher(a, "Frequency analyzer", "the strongest carriers in the span, with a Tune button", 0, nullptr, 3);
    launcher(a, "Sweep scanner", "a fast sweep of a wide band", 0, &a.sweepOpen, -1);
    launcher(a, "Interference hunter", "find a noise source with a loud and quiet meter", 0, &a.rfiOpen, -1);
    launcher(a, "ISM sensors", "weather stations, meters and remotes that use a known protocol", 28, nullptr, -1);
    launcher(a, "Protocol lab", "work out an unknown signal: modulation, baud rate, fields", 0, &a.protolabOpen, -1);
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("Logging and alerts"));
    launcher(a, "Drive log", "networks and devices with a position, to a WiGLE CSV or KML file", 0, nullptr, 4);
    launcher(a, "Messages", "every decoded message in one database", 0, &a.msgdbOpen, -1);
    launcher(a, "Flight history", "what flies over, where it goes, and your dossier on each aircraft", 0, &a.flightsOpen, -1);
    launcher(a, "Watchlists", "an alert when a listed call sign, frequency or word is heard", 0, &a.watchlistOpen, -1);
}

void startHint(App& a, int family, const char* text) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr(text));
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton(tr("Select it"))) selectMode(a, family);
}

void wifiAlerts(App& a) {
    const dect2::WifiTelemetry& w = a.rx.wifi;
    if (w.blocksOk == 0 && w.aps.empty()) { startHint(a, 54, "Select the Wi-Fi networks mode and start the receiver: the alarms read what it hears."); return; }
    const int64_t now = (int64_t)std::time(nullptr);
    const rt::DeauthAlarm al = rt::deauthAlarm(w.deauthLog, now);
    ImGui::TextColored(pal::heading(), "%s", tr("Deauthentication flood"));
    if (al.active) {
        ImGui::TextColored(pal::badRed(), "%s", tr("ALARM"));
        ImGui::SameLine();
        ImGui::Text("%d %s %s %s", al.frames, tr("deauthentication or disassociation frames in 10 s, most of them from"), al.bssid.c_str(),
                    al.broadcast ? tr("to every device") : (std::string(tr("to ")) + al.target).c_str());
    } else {
        ImGui::TextColored(pal::okGreen(), "%s", tr("quiet"));
        ImGui::SameLine();
        ImGui::TextDisabled("%d %s", al.frames, tr("frames in the last 10 s (the alarm starts at 20)"));
    }
    ImGui::TextDisabled("%llu %s, %llu %s", (unsigned long long)w.deauths, tr("deauthentication"), (unsigned long long)w.disassocs, tr("disassociation frames heard in all"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("A few are normal (a phone leaving a network). A steady stream aimed at everyone, or at one device, is how a connection is knocked off. "
                                 "Protected management frames (802.11w) make such frames ignored by devices that use them. Needs the radio on the channel of the network."));
    ImGui::PopTextWrapPos();
    if (!w.deauthLog.empty() && ImGui::BeginTable("##deauth", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, 110 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Age")); ImGui::TableSetupColumn(tr("Kind")); ImGui::TableSetupColumn(tr("From")); ImGui::TableSetupColumn(tr("To"));
        ImGui::TableSetupColumn(tr("Ch"));
        ImGui::TableHeadersRow();
        for (const auto& d : w.deauthLog) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%llds", (long long)(now - d.wall));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.disassoc ? "disassoc" : "deauth");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.source.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.dest.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", d.channel);
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextColored(pal::heading(), "%s", tr("Evil twin check"));
    const auto groups = rt::findTwins(w.aps);
    if (groups.empty()) ImGui::TextColored(pal::okGreen(), "%s", tr("no network name is shared in a suspicious way"));
    for (const auto& g : groups) {
        ImGui::PushStyleColor(ImGuiCol_Text, g.severity >= 2 ? pal::badRed() : pal::warnAmber());
        ImGui::Text("%s  \"%s\"", g.severity >= 2 ? tr("CHECK") : tr("note"), g.ssid.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g.why.c_str());
        for (size_t i : g.aps) {
            const dect2::WifiAp& ap = w.aps[i];
            ImGui::Text("      %s  ch %d  %s  %s  %.0f dBFS", ap.bssid.c_str(), ap.channel, dect2::wifi::securityName(ap.security, ap.enterprise), ap.vendor.c_str(), ap.rssi);
        }
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The same name on several addresses is normal in a mesh, an office or a hotel. It is worth a look when it is open on one address and protected on another."));
    ImGui::PopTextWrapPos();
}

void bluetooth(App& a) {
    const dect2::BleTelemetry& b = a.rx.ble;
    if (b.devices.empty() && b.blocksOk == 0) { startHint(a, 51, "Select the Bluetooth LE mode and start the receiver: the check reads the device names it hears."); return; }
    ImGui::TextColored(pal::heading(), "%s", tr("Serial module names"));
    int n = 0;
    for (const auto& d : b.devices) {
        std::string why;
        if (!rt::skimmerName(d.name, &why)) continue;
        n++;
        ImGui::TextColored(pal::warnAmber(), "%s", d.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s  %.0f dBFS  %llu %s", d.addr.c_str(), d.rssi, (unsigned long long)d.packets, tr("packets"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", why.c_str());
    }
    if (!n) ImGui::TextColored(pal::okGreen(), "%s", tr("no device with a serial module name"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Cheap serial Bluetooth modules (HC-05, HC-06, HC-08, JDY and similar) come with a default name. Some card skimmers use one, so phone apps that look for "
                                 "skimmers list them. So do many hobby projects, so a match is a reason to look at the device, not a finding. A strong level means it is close."));
    ImGui::PopTextWrapPos();
}

void analyzer(App& a) {
    ImGui::SetNextItemWidth(110 * gUi); if (ImGui::SliderInt(tr("lines"), &S.topN, 4, 30)) save();
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi); if (ImGui::SliderFloat(tr("dB above floor"), &S.overDb, 5.f, 40.f, "%.0f")) save();
    ImGui::SameLine(); ImGui::Checkbox(tr("Hold"), &S.hold);
    ImGui::TextDisabled("%s", tr("Go to"));
    for (const auto& p : rt::subGhzPresets()) {
        const float w = ImGui::CalcTextSize(tr(p.name)).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + w < right) ImGui::SameLine();   // wrap to the next line when the next button does not fit
        ImGui::PushID(p.name);
        if (ImGui::SmallButton(tr(p.name))) tuneTo(a, p.mhz * 1e6);
        ImGui::PopID();
    }
    if (!a.engine.running()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("Start the receiver to see the strongest carriers in the span.")); ImGui::PopTextWrapPos(); return; }
    const std::vector<float>& d = a.smooth.size() == a.spec.dbfs.size() && !a.smooth.empty() ? a.smooth : a.spec.dbfs;
    const double centre = a.engine.captureCentreHz() > 0 ? a.engine.captureCentreHz() : a.tune.centerHz;
    const double fs = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    if (!S.hold) { S.carriers = rt::findCarriers(d, centre, fs, S.topN, S.overDb); S.carrierCentre = centre; }
    ImGui::TextDisabled("%s %.3f MHz, %s %.2f MHz", tr("span centre"), S.carrierCentre / 1e6, tr("width"), fs / 1e6);
    if (S.carriers.empty()) { ImGui::TextDisabled("%s", tr("no carrier stands out in this span")); return; }
    if (ImGui::BeginTable("##carriers", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn(tr("Level dBFS")); ImGui::TableSetupColumn(tr("Over floor"));
        ImGui::TableSetupColumn(tr("Width kHz")); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        int row = 0;
        for (const rt::Carrier& c : S.carriers) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.4f", c.hz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", c.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", c.overFloorDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", c.widthHz / 1e3);
            ImGui::TableNextColumn();
            ImGui::PushID(row++);
            if (ImGui::SmallButton(tr("Tune"))) tuneTo(a, c.hz);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void driveLog(App& a) {
    (void)a;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Logs the Wi-Fi networks (and, if you ask, Bluetooth devices) heard while the Wi-Fi or Bluetooth mode runs, with a position, to a file on this computer. "
                                 "Nothing is uploaded; sending a file anywhere is up to you."));
    ImGui::PopTextWrapPos();
    ImGui::TextDisabled("%s", tr("Position"));
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Receiver position"), S.gpsMode == GpsReceiver)) { S.gpsMode = GpsReceiver; stopGps(); save(); }
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("GPS (NMEA)"), S.gpsMode == GpsNmea)) { S.gpsMode = GpsNmea; save(); startGps(); }
    if (S.gpsMode == GpsNmea) {
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::InputText(tr("serial port or NMEA file"), S.nmeaPath, sizeof S.nmeaPath, ImGuiInputTextFlags_EnterReturnsTrue)) { save(); startGps(); }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Open"))) { save(); startGps(); }
        dect2::geo::GpsFix f;
        if (S.gps.latest(f)) ImGui::TextColored(pal::okGreen(), "%s %.5f, %.5f (%d %s)", tr("fix"), f.lat, f.lon, f.sats, tr("satellites"));
        else ImGui::TextDisabled("%s", S.gps.running() ? S.gps.status().c_str() : tr("no fix yet"));
    } else {
        double lat, lon, alt;
        if (position(lat, lon, alt)) ImGui::TextDisabled("%s %.5f, %.5f", tr("fixed position"), lat, lon);
        else ImGui::TextDisabled("%s", tr("the receiver position is not set (Tools > Map, or the setup)"));
    }
    if (ImGui::Checkbox(tr("Include Bluetooth devices (personal devices; off by default)"), &S.includeBle)) { S.log.setBle(S.includeBle); save(); }
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::InputFloat(tr("level offset, dB (the radio's level is relative; this turns it into dBm for the file)"), &S.rssiOffset, 0, 0, "%.0f")) save();
    ImGui::Spacing();
    if (ImGui::Button(S.logging ? tr("Stop logging") : tr("Start logging"))) S.logging = !S.logging;
    ImGui::SameLine();
    ImGui::TextDisabled("%zu %s, %zu %s", S.log.wifiCount(), tr("networks"), S.log.bleCount(), tr("Bluetooth devices"));
    if (!S.status.empty() && S.logging) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", S.status.c_str()); }
    ImGui::BeginDisabled(S.log.size() == 0);
    if (ImGui::Button(tr("Save WiGLE CSV ..."))) {
        const std::string p = plat::saveFileDialog("saurcom-drive.csv");
        if (!p.empty()) S.status = writeFile(p, S.log.wigleCsv(S.rssiOffset)) ? "saved " + p : "could not write the file";
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Save KML ..."))) {
        const std::string p = plat::saveFileDialog("saurcom-drive.kml");
        if (!p.empty()) S.status = writeFile(p, S.log.kml()) ? "saved " + p : "could not write the file";
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) S.log.clear();
    ImGui::EndDisabled();
    if (!S.logging && !S.status.empty()) ImGui::TextDisabled("%s", S.status.c_str());
}

} // namespace

void radioToolsTick(App& a) {
    load();
    feedLog(a);
}

void radioToolsWindow(App& a) {
    radioToolsTick(a);
    if (!a.radioToolsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(820 * gUi, 520 * gUi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("Radio tools"), &a.radioToolsOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("##rtabs")) {
        static const char* names[] = {"Overview", "Wi-Fi alerts", "Bluetooth", "Frequency analyzer", "Drive log"};
        for (int i = 0; i < 5; i++) {
            const bool want = S.wantTab == i;
            if (ImGui::BeginTabItem(tr(names[i]), nullptr, want ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (i == 0) overview(a);
                else if (i == 1) wifiAlerts(a);
                else if (i == 2) bluetooth(a);
                else if (i == 3) analyzer(a);
                else driveLog(a);
                ImGui::EndTabItem();
            }
            if (want) S.wantTab = -1;
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void radioToolsDevOpen(App& a, int tab) {
    a.radioToolsOpen = true;
    S.wantTab = tab;
}
