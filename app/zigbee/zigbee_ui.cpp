// Zigbee / IEEE 802.15.4 screens: the PAN list (PAN ID, extended PAN ID, channel, coordinator, devices, level, packets, last seen), the device list
// (short and extended address, vendor from the OUI, role, level), the live frame log (time, channel, frame type, source > destination, length, FCS)
// with a packets-per-second plot, the channel picker (11 - 26) and the channel scan.
// Receive only: nothing is ever sent, joined or answered; encrypted payloads are not decrypted (the network key is not obtainable here), only frames
// sent in the clear are shown in full.
#include "app.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/zigbee/zigbee_tel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 networks, 1 devices, 2 frames
    bool loaded = false, scan = false;
    int dwellSec = 5;
    double lastHop = 0;
    char filter[64] = "";
    float minRssi = -120;
    std::string sel;                     // "pan/address" of the device clicked: the frame log shows only its frames
    int selPan = -1;                     // the PAN clicked in the network list: the device list and the log show only it
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.scan = plat::prefs().getB("zbScan", false);
    S.dwellSec = std::max(1, std::min(60, (int)plat::prefs().getI("zbDwell", 5)));
}

bool live(const App& a) { return a.rx.standard == 66; }   // the engine reports its standard code minus one

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string panText(uint16_t p) { char b[8]; snprintf(b, sizeof b, "0x%04X", p); return b; }
std::string devKey(const dect2::ZbDevice& d) { return panText(d.pan) + "/" + (d.shortAddr.empty() ? d.extAddr : d.shortAddr); }

bool passes(const dect2::ZbDevice& d) {
    if (d.rssi < S.minRssi) return false;
    if (S.selPan >= 0 && d.pan != S.selPan) return false;
    if (!S.filter[0]) return true;
    const std::string f = lower(S.filter);
    return lower(d.shortAddr).find(f) != std::string::npos || lower(d.extAddr).find(f) != std::string::npos || lower(d.vendor).find(f) != std::string::npos ||
           lower(d.role).find(f) != std::string::npos || lower(panText(d.pan)).find(f) != std::string::npos;
}

void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: Eye of ZorCom never transmits, joins or answers a network. Encrypted payloads are not decrypted (the network key is not obtainable here); "
                        "only frames sent in the clear are shown in full: beacons, MAC commands, headers and the few unencrypted messages. Needs a fast radio "
                        "(4 Msps or more, 2.4 GHz: not an RTL-SDR).");
    ImGui::PopTextWrapPos();
}

void filters(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(160 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##zbf", "address, vendor, role, PAN", S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Level above"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::SliderFloat("##zbr", &S.minRssi, -120, 0, "%.0f dBFS");
    flowNext(10 * gUi);
    if (S.selPan >= 0) {
        ImGui::TextColored(pal::okGreen(), "PAN %s only", panText((uint16_t)S.selPan).c_str());
        ImGui::SameLine(0, 5 * gUi);
        if (ImGui::SmallButton("Show all")) S.selPan = -1;
        flowNext(10 * gUi);
    }
    if (ImGui::Button("Clear")) a.engine.zigbee().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every network, device and frame heard so far");
    flowBreak();
}

void networksView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the networks around you"); return; }
    const dect2::ZigbeeTelemetry& t = a.rx.zigbee;
    if (t.ppsHist.size() >= 2) {
        ImGui::TextDisabled("Frames per second");
        ImGui::PlotLines("##zbpps", t.ppsHist.data(), (int)t.ppsHist.size(), 0, nullptr, 0, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, 44 * gUi));
    }
    if (!ImGui::BeginTable("##zbpan", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("PAN ID"); ImGui::TableSetupColumn("Extended PAN ID"); ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("Coordinator");
    ImGui::TableSetupColumn("Devices"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Packets"); ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& p : t.pans) {
        ImGui::TableNextRow();
        ImGui::PushID((int)(p.pan * 32 + p.channel));
        const double ago = std::max(0.0, t.timeSec - p.lastSec);
        const ImVec4 c = ago < 3 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.selPan == p.pan, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selPan = S.selPan == p.pan ? -1 : p.pan;
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", panText(p.pan).c_str()); ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s\nclick to show only this network's devices and frames", p.zigbee ? "Zigbee network" : "IEEE 802.15.4 network (no Zigbee header seen)",
                                                      p.stackProfile >= 0 ? (p.stackProfile == 2 ? ", stack profile 2 (Zigbee PRO)" : p.stackProfile == 1 ? ", stack profile 1" : "") : "", p.joinOpen ? "\njoining is open" : "");
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.extPan.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); if (p.channel) ImGui::Text("%d", p.channel); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.coordinator.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", p.devices);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", p.rssi);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)p.packets);
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago);
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.pans.empty()) ImGui::TextDisabled("nothing heard yet");
}

void devicesView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the devices around you"); return; }
    filters(a);
    const dect2::ZigbeeTelemetry& t = a.rx.zigbee;
    if (!ImGui::BeginTable("##zbdev", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Short"); ImGui::TableSetupColumn("Extended"); ImGui::TableSetupColumn("Vendor"); ImGui::TableSetupColumn("PAN"); ImGui::TableSetupColumn("Role");
    ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Packets"); ImGui::TableSetupColumn("Last seen / last frame", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int shown = 0;
    for (const auto& d : t.devices) {
        if (!passes(d)) continue;
        shown++;
        ImGui::TableNextRow();
        const std::string key = devKey(d);
        ImGui::PushID(key.c_str());
        const double ago = std::max(0.0, t.timeSec - d.lastSec);
        const ImVec4 c = ago < 3 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.sel == key, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = S.sel == key ? std::string() : key;
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", d.shortAddr.empty() ? "-" : d.shortAddr.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(d.extAddr.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.vendor.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(panText(d.pan).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.role.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.0f", d.rssi);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.packets);
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago); ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", d.last.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.devices.empty()) ImGui::TextDisabled("nothing heard yet");
    else if (!shown) ImGui::TextDisabled("no device passes the filter");
}

void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the frames"); return; }
    const dect2::ZigbeeTelemetry& t = a.rx.zigbee;
    const auto& log = t.log;
    if (!S.sel.empty() || S.selPan >= 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(pal::okGreen(), "Showing %s only", !S.sel.empty() ? S.sel.c_str() : panText((uint16_t)S.selPan).c_str());
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton("Show all")) { S.sel.clear(); S.selPan = -1; }
    }
    const std::string key = S.sel;
    std::vector<size_t> rows;
    for (size_t i = 0; i < log.size(); i++) {
        const auto& f = log[i];
        if (S.selPan >= 0 && f.pan != S.selPan) continue;
        if (!key.empty()) {
            const std::string pfx = panText(f.pan) + "/";
            if (key != pfx + f.src && key != pfx + f.dst) continue;
        }
        rows.push_back(i);
    }
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (size_t i : rows) {
            const auto& f = log[i];
            char b[600];
            snprintf(b, sizeof b, "%s\t%d\t%s\t%s > %s\t%d\t%s\t%s\n", clock(f.wallTime).c_str(), f.channel, f.type.c_str(), f.src.c_str(), f.dst.c_str(), f.len, f.fcsOk ? "ok" : "bad", f.info.c_str());
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("%.0f frames/s", t.pps);
    if (!ImGui::BeginTable("##zblog", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time UTC"); ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Source > destination");
    ImGui::TableSetupColumn("Len"); ImGui::TableSetupColumn("FCS"); ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& f = log[rows[(size_t)i]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(f.wallTime).c_str());
            ImGui::TableNextColumn(); if (f.channel) ImGui::Text("%d", f.channel); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.type.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%s > %s", f.src.c_str(), f.dst.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%d", f.len);
            ImGui::TableNextColumn(); if (f.fcsOk) ImGui::TextColored(pal::okGreen(), "ok"); else ImGui::TextDisabled("bad");
            ImGui::TableNextColumn(); if (f.encrypted) ImGui::TextDisabled("%s", f.info.c_str()); else ImGui::TextUnformatted(f.info.c_str());
        }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("no frames yet");
    else if (rows.empty()) ImGui::TextDisabled("no frame passes the selection");
}

void hopTo(App& a, int ch) {
    const double mhz = dect2::zigbee::channelMhz(ch);
    a.freqMhz = mhz;
    a.tune.centerHz = mhz * 1e6;
    a.engine.retune(a.tune);   // no reset: the tables stay
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    loadState();
    if (!a.engine.running() || !S.scan || a.devices[a.devIdx].kind == DeviceInfo::File) { S.lastHop = ImGui::GetTime(); return; }
    if (ImGui::GetTime() - S.lastHop < S.dwellSec) return;
    S.lastHop = ImGui::GetTime();
    const int cur = dect2::zigbee::channelOf(a.freqMhz);
    hopTo(a, cur >= dect2::zigbee::kLastChannel || cur < dect2::zigbee::kFirstChannel ? dect2::zigbee::kFirstChannel : cur + 1);
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Channel");
    const int cur = dect2::zigbee::channelOf(a.freqMhz);
    char label[40];
    if (cur) snprintf(label, sizeof label, "%d (%.0f MHz)", cur, dect2::zigbee::channelMhz(cur)); else snprintf(label, sizeof label, "other");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(140 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##zbch", label)) {
        for (int c = dect2::zigbee::kFirstChannel; c <= dect2::zigbee::kLastChannel; c++) {
            char b[48];
            snprintf(b, sizeof b, "%d  %.0f MHz", c, dect2::zigbee::channelMhz(c));
            if (ImGui::Selectable(b, c == cur)) { tuneFreq(a, dect2::zigbee::channelMhz(c)); retune = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("802.15.4 channels 11 to 26, 2 MHz wide, 5 MHz apart from 2405 MHz. A radio takes in one channel at a time at 4 Msps;\n"
                                                  "channels 15, 20, 25 and 26 are the ones Zigbee networks usually avoid Wi-Fi with. Single-channel mode listens to the one chosen.");
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Checkbox("scan", &S.scan)) { plat::prefs().setB("zbScan", S.scan); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step through channels 11 to 26, listening to each for the dwell time (a radio that takes in a single 2 MHz channel misses what is sent\n"
                                                  "on the others meanwhile). Off: single channel.");
    if (S.scan) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(70 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderInt("##zbdw", &S.dwellSec, 1, 60, "%d s")) { plat::prefs().setI("zbDwell", S.dwellSec); plat::prefs().flush(); }
    }
}

void tab(App& a) {
    subNav("zbv", S.view, {"Networks", "Devices", "Frames"});
    if (S.view == 0) networksView(a); else if (S.view == 1) devicesView(a); else framesView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::ZigbeeTelemetry& t = a.rx.zigbee;
    if (!ImGui::BeginTable("##zbl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Network / device", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Level");
    for (const auto& p : t.pans) {
        ImGui::TableNextRow();
        ImGui::PushID((int)(p.pan * 32 + p.channel));
        const bool recent = t.timeSec - p.lastSec < 3;
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.selPan == p.pan, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selPan = S.selPan == p.pan ? -1 : p.pan;
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(recent ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "PAN %s", panText(p.pan).c_str()); ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("channel %d, %d devices\n%s", p.channel, p.devices, p.extPan.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", p.rssi);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::ZigbeeTelemetry& t = a.rx.zigbee;
    kvRow(a, "state", "%s", dect2::zigbeeSummary(t).c_str());
    if (t.channel) kvRow(a, "channel", "%d (%.0f MHz)%s", t.channel, dect2::zigbee::channelMhz(t.channel), S.scan ? ", scanning" : "");
    kvRow(a, "signal", "chip SNR %.1f dB, offset %+.1f kHz", t.snrDb, t.cfoHz / 1e3);
    kvRow(a, "frames", "%llu good, %llu failed FCS, %llu preambles, %llu encrypted", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.preambles, (unsigned long long)t.encryptedFrames);
    kvRow(a, "by type", "beacon %llu, data %llu, ack %llu, command %llu", (unsigned long long)t.perType[0], (unsigned long long)t.perType[1], (unsigned long long)t.perType[2], (unsigned long long)t.perType[3]);
    std::string ch;
    for (int i = 0; i < 16; i++) if (t.perChannel[i]) { char b[32]; snprintf(b, sizeof b, "%s%d: %llu", ch.empty() ? "" : ", ", 11 + i, (unsigned long long)t.perChannel[i]); ch += b; }
    kvRow(a, "by channel", "%s", ch.empty() ? "-" : ch.c_str());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    receiveNote();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Frames", !on ? 0 : a.rx.zigbee.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::zigbeeSummary(a.rx.zigbee).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Zigbee / 802.15.4";
    if (live(a)) l2 = dect2::zigbeeSummary(a.rx.zigbee);
}

} // namespace

extern const ModeUi kZigbeeUi;
const ModeUi kZigbeeUi = {
    .sideTitle = "NETWORKS",
    .tabName = "Zigbee",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
