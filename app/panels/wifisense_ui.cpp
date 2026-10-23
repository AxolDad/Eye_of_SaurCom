// View > WiFi sensing: presence, motion, breathing and heart rate in each room an ESP32 CSI node watches (RuView firmware, which sends
// its channel state frames over UDP). core/src/net/wifisense_rx.cpp listens; the analysis is the Rust crate rust/onair_wifisense.
#include "../app.h"
#include "dect2/net/wifisense_rx.h"
#include <algorithm>
#include <chrono>
#include <string>

namespace {

struct State {
    bool loaded = false;
    int port = dect2::WifiSenseListener::kDefaultPort;
    std::string err;
};
State S;

int64_t wallMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

void listen(bool on) {
    auto& L = dect2::WifiSenseListener::get();
    S.err.clear();
    if (on) L.start(S.port, S.err);
    else L.stop();
}

// a rate with its confidence, or a dash while unknown
void rate(double bpm, double conf) {
    if (bpm <= 0) { ImGui::TextDisabled("-"); return; }
    const ImVec4 c = conf >= 0.7 ? ImVec4(0.4f, 0.9f, 0.4f, 1) : conf >= 0.4 ? ImVec4(0.95f, 0.8f, 0.3f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1);
    ImGui::TextColored(c, "%.0f  (%.0f%%)", bpm, conf * 100);
}

} // namespace

void wifiSenseWindow(App& a) {
    if (!S.loaded) {
        S.loaded = true;
        S.port = (int)plat::prefs().getI("wifiSensePort", dect2::WifiSenseListener::kDefaultPort);
        if (plat::prefs().getB("wifiSenseListen", false)) listen(true);
    }
    if (!a.wifiSenseOpen) return;
    auto& L = dect2::WifiSenseListener::get();
    ImGui::SetNextWindowSize(ImVec2(640 * gUi, 320 * gUi), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("WiFi sensing"), &a.wifiSenseOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    ImGui::TextWrapped("%s", tr("People in a room change the WiFi channel between an ESP32 node and its access point. Flash the nodes with "
                                "RuView's CSI firmware and point them at this computer's address."));
    bool on = L.running();
    if (ImGui::Checkbox(tr("Listen"), &on)) {
        plat::prefs().setB("wifiSenseListen", on);
        listen(on);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputInt(tr("UDP port"), &S.port, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
        S.port = std::clamp(S.port, 1, 65535);
        plat::prefs().setI("wifiSensePort", S.port);
        if (L.running()) listen(true);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear"))) L.clear();
    if (!S.err.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", S.err.c_str());

    const auto nodes = L.nodes();
    if (nodes.empty()) {
        ImGui::TextDisabled("%s", L.running() ? tr("Listening; no node heard yet.") : tr("Not listening."));
    } else if (ImGui::BeginTable("##nodes", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr("Node"));
        ImGui::TableSetupColumn(tr("Room"));
        ImGui::TableSetupColumn(tr("Motion"));
        ImGui::TableSetupColumn(tr("Breaths / min"));
        ImGui::TableSetupColumn(tr("Heart / min"));
        ImGui::TableSetupColumn(tr("Signal"));
        ImGui::TableSetupColumn(tr("Last heard"));
        ImGui::TableHeadersRow();
        const int64_t now = wallMs();
        for (const auto& n : nodes) {
            const double age = (double)(now - n.heardMs) / 1000.0;
            const bool stale = age > 5;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", n.s.node_id);
            if (ImGui::IsItemHovered() && !n.from.empty()) ImGui::SetTooltip("%s", n.from.c_str());
            ImGui::TableNextColumn();
            if (stale) ImGui::TextDisabled("%s", tr("unknown"));
            else if (n.s.presence) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%s", tr("occupied"));
            else ImGui::TextUnformatted(tr("empty"));
            ImGui::TableNextColumn();
            ImGui::ProgressBar((float)std::min(1.0, n.s.motion / 0.1), ImVec2(80 * gUi, 0), "");
            ImGui::TableNextColumn();
            rate(stale ? 0 : n.s.breathing_bpm, n.s.breathing_conf);
            ImGui::TableNextColumn();
            rate(stale ? 0 : n.s.heart_bpm, n.s.heart_conf);
            ImGui::TableNextColumn();
            ImGui::Text("%d dBm, %u MHz", n.s.rssi, n.s.freq_mhz);
            ImGui::TableNextColumn();
            if (age < 1) ImGui::TextUnformatted(tr("now"));
            else ImGui::Text("%.0f s", age);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", tr("Breathing and heart rate need a still person; they are estimates, not medical readings."));
    ImGui::End();
}
