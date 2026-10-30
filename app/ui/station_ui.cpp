// Tools > My station: the one place to say where this station is. A paste box that understands coordinates, locators and map links, fields
// for the numbers, the source of the current position, and a GPS (a serial NMEA receiver or gpsd) that can set it once or follow it.
// Nothing is looked up on the internet; the only network traffic is the connection to the gpsd host the user names.
#include "station_ui.h"
#include "../state/gps_serial.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/source/rig_setup.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using dect2::geo::GpsFix;
using dect2::geo::StationPos;

namespace {

struct State {
    bool loaded = false;
    // the number fields; refilled from the station when it changes elsewhere (a different generation) and no field is being edited
    char lat[32] = "", lon[32] = "", alt[32] = "", ant[32] = "";
    unsigned gen = 0;
    std::string applyErr;       // the last Apply's reason, shown under the fields
    char paste[256] = "";
    // GPS
    int gpsMode = 0;            // 0 none, 1 serial NMEA, 2 gpsd
    char port[256] = "";
    int baudIdx = 1;
    char host[96] = "127.0.0.1";
    int gpsdPort = 2947;
    bool follow = false;
    int active = 0;             // the reader that is running: 0 none, 1 serial, 2 gpsd
    SerialNmea serial;
    dect2::geo::GpsdClient gpsd;
    double lastApply = -1;      // ImGui time of the last follow update
    std::string gpsMsg;
    std::vector<dect2::SerialPortInfo> ports;
    bool portsListed = false;
    // Locate (Wi-Fi), macOS
    bool locBusy = false;
    std::string locMsg;
};
State S;

const int kBauds[] = {4800, 9600, 19200, 38400, 57600, 115200};

void gpsStop() { S.serial.stop(); S.gpsd.stop(); S.active = 0; }

void gpsStart() {
    gpsStop();
    S.gpsMsg.clear();
    if (S.gpsMode == 1) {
        if (!S.port[0]) { S.gpsMsg = tr("Choose a serial port first."); return; }
        S.serial.start(S.port, kBauds[std::clamp(S.baudIdx, 0, 5)]);
        S.active = 1;
    } else if (S.gpsMode == 2) {
        if (!S.gpsd.start(S.host, S.gpsdPort)) { S.gpsMsg = tr("The host or the port is not valid."); return; }
        S.active = 2;
    }
}

void saveGps() {
    plat::Prefs& p = plat::prefs();
    p.setI("stationGpsMode", S.gpsMode); p.setS("stationGpsPort", S.port); p.setI("stationGpsBaud", kBauds[std::clamp(S.baudIdx, 0, 5)]);
    p.setS("stationGpsHost", S.host); p.setI("stationGpsPortNum", S.gpsdPort); p.setB("stationGpsFollow", S.follow);
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.gpsMode = (int)std::clamp(p.getI("stationGpsMode", 0), 0L, 2L);
    snprintf(S.port, sizeof S.port, "%s", p.getS("stationGpsPort", "").c_str());
    const long baud = p.getI("stationGpsBaud", 9600);
    S.baudIdx = 1;
    for (int i = 0; i < 6; i++) if (kBauds[i] == baud) S.baudIdx = i;
    snprintf(S.host, sizeof S.host, "%s", p.getS("stationGpsHost", "127.0.0.1").c_str());
    S.gpsdPort = (int)std::clamp(p.getI("stationGpsPortNum", 2947), 1L, 65535L);
    S.follow = p.getB("stationGpsFollow", false);
    if (S.follow && S.gpsMode != 0) gpsStart();   // a station that follows its GPS picks the receiver up again at start
}

bool currentFix(GpsFix& f) {
    if (S.active == 1) return S.serial.latest(f);
    if (S.active == 2) return S.gpsd.latest(f);
    return false;
}

std::string gpsStatus() {
    if (S.active == 1) return S.serial.status();
    if (S.active == 2) return S.gpsd.status();
    return S.gpsMsg.empty() ? std::string(tr("not connected")) : S.gpsMsg;
}

StationPos posFromFix(const GpsFix& f) {
    StationPos p;
    p.lat = f.lat; p.lon = f.lon;
    if (f.mode >= 3 && dect2::geo::validAlt(f.altM)) p.altM = f.altM;
    p.source = "gps";
    return p;
}

// "52.1", "52,1" or " -1.3 " as a number; empty text is "no value" (ok, NaN)
bool readNumber(const char* text, double& v, bool& empty) {
    std::string t = text;
    while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
    while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
    empty = t.empty();
    v = NAN;
    if (empty) return true;
    for (char& c : t) if (c == ',') c = '.';
    char* end = nullptr;
    v = strtod(t.c_str(), &end);
    return end && *end == 0 && std::isfinite(v);
}

void fillFields(const StationPos& p) {
    S.lat[0] = S.lon[0] = S.alt[0] = S.ant[0] = 0;
    if (!p.valid()) return;
    snprintf(S.lat, sizeof S.lat, "%.5f", p.lat);
    snprintf(S.lon, sizeof S.lon, "%.5f", p.lon);
    if (!std::isnan(p.altM)) snprintf(S.alt, sizeof S.alt, "%.0f", p.altM);
    if (!std::isnan(p.antM)) snprintf(S.ant, sizeof S.ant, "%.1f", p.antM);
}

std::string coordText(double lat, double lon) { char b[64]; snprintf(b, sizeof b, "%.5f, %.5f", lat, lon); return b; }

void pollLocate(App& a) {
    if (!S.locBusy) return;
    double la = 0, lo = 0, acc = 0;
    std::string msg;
    const int st = plat::locateState(la, lo, acc, msg);
    if (st == 2) {
        StationPos p; p.lat = la; p.lon = lo; p.source = "wifi";
        std::string err;
        char b[128];
        if (setStationPos(a, p, &err)) snprintf(b, sizeof b, tr("position found (about %.0f m)"), acc);
        else snprintf(b, sizeof b, "%s", err.c_str());
        S.locMsg = b; S.locBusy = false;
    } else if (st == 3) { S.locMsg = msg; S.locBusy = false; }
}

// follow the GPS: runs every frame, window open or not
double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void followTick(App& a) {
    if (!S.follow || !S.active) return;
    GpsFix f;
    if (!currentFix(f)) return;   // no fix never moves the station
    const StationPos cur = stationPos();
    const double now = nowSec();
    if (!dect2::geo::followShouldMove(cur.valid(), cur.lat, cur.lon, f.lat, f.lon, now, S.lastApply)) return;
    if (setStationPos(a, posFromFix(f))) S.lastApply = now;
}

void section(const char* title) {
    ImGui::Spacing();
    ImGui::TextColored(pal::accent(), "%s", title);
    ImGui::Separator();
}

void currentBlock() {
    const StationPos p = stationPos();
    if (p.valid()) {
        ImGui::Text("%s", coordText(p.lat, p.lon).c_str());
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("(%s %s)", tr("locator"), dect2::toMaidenhead(p.lat, p.lon, 6).c_str());
        ImGui::TextDisabled("%s: %s", tr("Source"), stationSourceLabel(p.source).c_str());
        if (p.source == "env") {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", tr("Taken from the environment of this container and not saved. Set it here to keep your own."));
            ImGui::PopTextWrapPos();
        }
    } else {
        ImGui::TextColored(pal::warnAmber(), "%s", tr("Not set"));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Paste coordinates, a locator or a map link below, type the numbers, or use a GPS. It stays on this computer: nothing is looked up on the internet."));
        ImGui::PopTextWrapPos();
        const std::string ee = stationEnvError();
        if (!ee.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::badRed(), "%s", ee.c_str()); ImGui::PopTextWrapPos(); }
    }
}

void fieldsBlock(App& a) {
    const unsigned g = stationGeneration();
    if (g != S.gen && !ImGui::IsAnyItemActive()) { S.gen = g; fillFields(stationPos()); S.applyErr.clear(); }
    const float w = 130 * gUi;
    double lat = NAN, lon = NAN, alt = NAN, ant = NAN;
    bool eLat, eLon, eAlt, eAnt;
    const bool okLat = readNumber(S.lat, lat, eLat) && !eLat && lat >= -90 && lat <= 90;
    const bool okLon = readNumber(S.lon, lon, eLon) && !eLon && lon >= -180 && lon <= 180;
    const bool okAlt = readNumber(S.alt, alt, eAlt) && (eAlt || dect2::geo::validAlt(alt));
    const bool okAnt = readNumber(S.ant, ant, eAnt) && (eAnt || (ant >= 0 && ant <= 1000));
    // empty while no position is set (never the placeholder), with a hint in the box; an empty box is not an error to shout about
    auto row = [&](const char* label, char* buf, size_t n, bool ok, const char* hint, const char* id, const char* ph) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(190 * gUi);
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint(id, ph, buf, n);
        if (!ok && buf[0]) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(pal::badRed(), "%s", hint); }
    };
    row(tr("Latitude (north +)"), S.lat, sizeof S.lat, okLat, tr("a number from -90 to 90"), "##stlat", tr("latitude"));
    row(tr("Longitude (east +)"), S.lon, sizeof S.lon, okLon, tr("a number from -180 to 180"), "##stlon", tr("longitude"));
    row(tr("Height above sea level (m)"), S.alt, sizeof S.alt, okAlt, tr("metres, -500 to 9000"), "##stalt", tr("unknown"));
    row(tr("Antenna above ground (m)"), S.ant, sizeof S.ant, okAnt, tr("metres, 0 to 1000"), "##stant", tr("unknown"));
    const bool all = okLat && okLon && okAlt && okAnt;
    ImGui::BeginDisabled(!all);
    if (ImGui::Button(tr("Apply"))) {
        StationPos p; p.lat = lat; p.lon = lon; p.altM = alt; p.antM = ant; p.source = "typed";
        S.applyErr.clear();
        if (!setStationPos(a, p, &S.applyErr)) { /* shown below */ }
        else { S.gen = stationGeneration(); fillFields(stationPos()); }
    }
    ImGui::EndDisabled();
    if (!S.applyErr.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(pal::badRed(), "%s", S.applyErr.c_str()); }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Leave a height empty if you do not know it. The heights help the pass planner and the propagation tools."));
    ImGui::PopTextWrapPos();
}

void gpsBlock(App& a) {
    int mode = S.gpsMode;
    bool changed = false;
    if (ImGui::RadioButton(tr("Serial GPS (NMEA)"), mode == 1)) { mode = 1; changed = true; }
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::RadioButton(tr("gpsd"), mode == 2)) { mode = 2; changed = true; }
    if (changed && mode != S.gpsMode) { gpsStop(); S.gpsMode = mode; saveGps(); }
    if (S.gpsMode == 1) {
        if (!S.portsListed) { S.ports = dect2::listSerialPorts(); S.portsListed = true; }
        ImGui::SetNextItemWidth(300 * gUi);
        const char* cur = S.port[0] ? S.port : tr("(choose a port)");
        if (ImGui::BeginCombo("##stport", cur)) {
            for (const auto& pi : S.ports) if (ImGui::Selectable(pi.display.c_str(), pi.path == S.port)) { snprintf(S.port, sizeof S.port, "%s", pi.path.c_str()); saveGps(); }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::SmallButton(tr("Refresh##stports"))) S.ports = dect2::listSerialPorts();
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::InputTextWithHint("##stportt", tr("or type a port or a file of NMEA sentences"), S.port, sizeof S.port)) saveGps();
        ImGui::SetNextItemWidth(110 * gUi);
        char bl[16]; snprintf(bl, sizeof bl, "%d", kBauds[std::clamp(S.baudIdx, 0, 5)]);
        if (ImGui::BeginCombo(tr("baud"), bl)) {
            for (int i = 0; i < 6; i++) { char b[16]; snprintf(b, sizeof b, "%d", kBauds[i]); if (ImGui::Selectable(b, i == S.baudIdx)) { S.baudIdx = i; saveGps(); } }
            ImGui::EndCombo();
        }
    } else if (S.gpsMode == 2) {
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::InputText(tr("host"), S.host, sizeof S.host)) saveGps();
        ImGui::SameLine(0, 8 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::InputInt(tr("port"), &S.gpsdPort, 0, 0)) { S.gpsdPort = std::clamp(S.gpsdPort, 1, 65535); saveGps(); }
    }
    if (S.gpsMode != 0) {
        if (S.active) { if (ImGui::Button(tr("Disconnect"))) gpsStop(); }
        else if (ImGui::Button(tr("Connect"))) { gpsStart(); saveGps(); }
        GpsFix f;
        const bool fix = currentFix(f);
        const std::string st = gpsStatus();
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextColored(fix ? pal::okGreen() : pal::warnAmber(), "%s", st.c_str());   // the text may come from the gpsd host: never a format string
        if (fix) ImGui::TextDisabled("%s", coordText(f.lat, f.lon).c_str());
        else if (S.active) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("No fix yet: the station position stays as it is until the GPS has one.")); ImGui::PopTextWrapPos(); }
        ImGui::BeginDisabled(!fix);
        if (ImGui::Button(tr("Use this fix once"))) {
            std::string err;
            if (!setStationPos(a, posFromFix(f), &err)) S.gpsMsg = err;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, 14 * gUi);
        if (ImGui::Checkbox(tr("Follow the GPS"), &S.follow)) { saveGps(); S.lastApply = -1; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("For a station on the move: the position follows the GPS, at most every 10 seconds and only when it has moved more than 20 m. Without a fix it does not change."));
        if (!S.gpsMsg.empty() && !S.active) ImGui::TextColored(pal::badRed(), "%s", S.gpsMsg.c_str());
    }
}

}   // namespace

bool stationPreview(const char* text, StationPos* out) {
    if (!text || !text[0]) return false;
    StationPos p;
    std::string err;
    if (dect2::geo::parseLocationText(text, p, err)) {
        char b[160];
        snprintf(b, sizeof b, tr("-> %.5f, %.5f (locator %s)"), p.lat, p.lon, dect2::toMaidenhead(p.lat, p.lon, 4).c_str());
        ImGui::TextColored(pal::okGreen(), "%s", b);
        if (out) *out = p;
        return true;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(pal::badRed(), "%s", err.c_str());
    ImGui::PopTextWrapPos();
    return false;
}

bool stationPasteBox(App& a, char* buf, size_t n, const char* id, float width) {
    if (width > 0) ImGui::SetNextItemWidth(width); else ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputTextWithHint(id, tr("Paste coordinates, a locator (IO91wm) or a Google / OpenStreetMap link"), buf, n, ImGuiInputTextFlags_EnterReturnsTrue);
    StationPos p;
    const bool ok = stationPreview(buf, &p);
    ImGui::BeginDisabled(!ok);
    const bool use = ImGui::Button((std::string(tr("Use this")) + "##use" + id).c_str());
    ImGui::EndDisabled();
    if ((enter || use) && ok) {
        if (p.source != "locator") p.source = "pasted";
        if (setStationPos(a, p)) { buf[0] = 0; return true; }
    }
    return false;
}

void stationOpenPage(App& a) { a.myStationOpen = true; }

void stationHint(App& a, const char* why) {
    if (receiverLocationSet()) return;
    ImGui::PushID(why);
    if (why && *why) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", why); ImGui::PopTextWrapPos(); }
    if (ImGui::SmallButton(tr("Set your location"))) stationOpenPage(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Open My station: paste coordinates, a locator or a map link, or use a GPS"));
    ImGui::PopID();
}

// every loop of the main program, drawn or not (a full-screen map, a minimised window): the GPS keeps the station where the vehicle is
void stationTick(App& a) {
    stationBindApp(a);   // the maps' "I'm here" saves through it
    load();
    pollLocate(a);
    followTick(a);
}

void myStationWindow(App& a) {
    if (!a.myStationOpen) return;
    ImGui::SetNextWindowSize(ImVec2(560 * gUi, 640 * gUi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("My station"), &a.myStationOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Where this station is. Ranges, maps, the pass planner and the propagation tools all use this one position."));
    ImGui::PopTextWrapPos();
    section(tr("Position"));
    currentBlock();
    section(tr("Paste a location"));
    stationPasteBox(a, S.paste, sizeof S.paste, "##stpaste");
    section(tr("Or type it"));
    fieldsBlock(a);
#ifdef __APPLE__
    ImGui::Spacing();
    if (ImGui::Button(S.locBusy ? tr("Locating...") : tr("Locate (Wi-Fi)")) && !S.locBusy) { plat::locateStart(); S.locBusy = true; S.locMsg = tr("asking the location service..."); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Ask the system for this computer's position (Wi-Fi positioning). macOS asks for permission the first time."));
    if (!S.locMsg.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", S.locMsg.c_str()); }
#endif
    section(tr("Use GPS"));
    gpsBlock(a);
    ImGui::End();
}

void stationChip(App& a) {
    const StationPos p = stationPos();
    const bool set = p.valid();
    const float h = ImGui::GetFrameHeight();
    // narrow window: the pin alone (the coordinates go first); an unset position keeps its words, it is the one thing to do
    const bool narrow = ImGui::GetWindowWidth() < 900 * gUi;
    char text[64] = "";
    if (set && !narrow) snprintf(text, sizeof text, "%.2f, %.2f", p.lat, p.lon);
    else if (!set && ImGui::GetWindowWidth() >= 640 * gUi) snprintf(text, sizeof text, "%s", tr("Set location"));
    const float pinW = h * 0.55f, pad = 6 * gUi;
    const float textW = text[0] ? ImGui::CalcTextSize(text).x + 4 * gUi : 0.f;
    ImGui::PushID("stationchip");
    const bool click = ImGui::InvisibleButton("##chip", ImVec2(pad + pinW + textW + pad, h));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 base = set ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : pal::accent();
    const ImU32 col = ImGui::GetColorU32(base);
    if (hov || !set) dl->AddRect(mn, mx, ImGui::GetColorU32(set ? ImGui::GetStyleColorVec4(ImGuiCol_Border) : pal::accent(0.8f)), 3 * gUi);
    // the pin: a ring on a point, drawn (no font has the glyph everywhere)
    const float cx = mn.x + pad + pinW * 0.5f, top = mn.y + h * 0.22f, r = pinW * 0.42f;
    dl->AddTriangleFilled(ImVec2(cx - r * 0.85f, top + r * 1.2f), ImVec2(cx + r * 0.85f, top + r * 1.2f), ImVec2(cx, mn.y + h * 0.80f), col);
    dl->AddCircleFilled(ImVec2(cx, top + r), r, col);
    dl->AddCircleFilled(ImVec2(cx, top + r), r * 0.4f, ImGui::GetColorU32(ImGuiCol_WindowBg));
    if (text[0]) dl->AddText(ImVec2(mn.x + pad + pinW + 4 * gUi, mn.y + (h - ImGui::GetFontSize()) * 0.5f), col, text);
    if (hov) {
        ImGui::BeginTooltip();
        if (set) {
            ImGui::Text("%s", coordText(p.lat, p.lon).c_str());
            ImGui::TextDisabled("%s: %s", tr("Source"), stationSourceLabel(p.source).c_str());
            if (!std::isnan(p.antM)) ImGui::TextDisabled(tr("Antenna %.1f m above ground"), p.antM);
            if (!std::isnan(p.altM)) ImGui::TextDisabled(tr("%.0f m above sea level"), p.altM);
            ImGui::TextDisabled("%s", tr("Click to change"));
        } else {
            ImGui::TextUnformatted(tr("Your location is not set."));
            ImGui::TextDisabled("%s", tr("Click to set it: paste coordinates, a locator or a map link, or use a GPS."));
        }
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    if (click) stationOpenPage(a);
}
