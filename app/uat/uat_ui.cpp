// UAT (978 MHz): aircraft table, FIS-B weather text, message log.
#include "../panels/photo_links_ui.h"
#include "../ui/typography.h"
#include "app.h"
#include "dect2/uat/uat_rx.h"
#include "dect2/uat/uat_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

struct State {
    bool loaded = false;
    uint32_t sel = 0xFFFFFFFFu;
    double pushedLat = 1e9, pushedLon = 1e9;
};
State S;

bool live(const App& a) { return a.rx.standard == 84; }

// UAT used to keep its own position (uatLat / uatLon). It is the station's now (state/station_loc.h); a position typed here by an older
// version becomes the station's once, when there is none yet.
void loadState(App& a) {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    if (!receiverLocationSet() && d.has("uatLat") && d.has("uatLon")) {
        dect2::geo::StationPos p;
        p.lat = d.getD("uatLat", 0); p.lon = d.getD("uatLon", 0); p.source = "legacy";
        setStationPos(a, p);   // refused (and nothing changes) when it is no position
    }
}

void tick(App& a) {
    loadState(a);
    if (!live(a)) { S.pushedLat = S.pushedLon = 1e9; return; }
    if (!receiverLocationSet()) return;   // ranges need the station: none from a placeholder
    double la, lo; receiverLocation(la, lo);
    if (la != S.pushedLat || lo != S.pushedLon) { a.engine.uat().setReference(la, lo); S.pushedLat = la; S.pushedLon = lo; }
}

void tuner(App& a, bool&) {
    ImGui::TextDisabled("Antenna position (for ranges)");
    receiverPositionFields(a);   // the station's position, as on the ADS-B screen
}

void aircraftTable(const dect2::UatTelemetry& t) {
    if (ImGui::BeginTable("##uata", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("ICAO"); ImGui::TableSetupColumn("Call"); ImGui::TableSetupColumn("Alt ft");
        ImGui::TableSetupColumn("Speed"); ImGui::TableSetupColumn("Hdg"); ImGui::TableSetupColumn("Range");
        ImGui::TableSetupColumn("Src"); ImGui::TableSetupColumn("Age");
        ImGui::TableHeadersRow();
        for (const auto& ac : t.aircraft) {
            ImGui::TableNextRow();
            char id[16]; snprintf(id, sizeof id, "%06X", ac.icao & 0xFFFFFF);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(id, S.sel == ac.icao, ImGuiSelectableFlags_SpanAllColumns)) S.sel = S.sel == ac.icao ? 0xFFFFFFFFu : ac.icao;
            if (ImGui::BeginPopupContextItem()) { photolinks::aircraftMenuItems(ac.icao & 0xFFFFFF, "", ac.callsign); ImGui::EndPopup(); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.callsign.empty() ? "-" : ac.callsign.c_str());
            ImGui::TableNextColumn(); ImGui::Text(ac.hasAlt ? "%d" : "-", ac.altFt);
            ImGui::TableNextColumn(); ImGui::Text(ac.hasSpeed ? "%d kt" : "-", ac.speedKt);
            ImGui::TableNextColumn(); ImGui::Text(ac.hasHeading ? "%.0f" : "-", ac.headingDeg);
            ImGui::TableNextColumn();
            if (ac.hasRange) ImGui::Text("%.1f NM", ac.distNm); else ImGui::TextUnformatted(ac.hasPos && !receiverLocationSet() ? kNoRange : "-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.tisb ? "TIS-B" : "ADS-B");
            ImGui::TableNextColumn(); ImGui::Text("%.0f s", ac.ageSec);
        }
        ImGui::EndTable();
    }
}

void tab(App& a) {
    const dect2::UatTelemetry& t = a.rx.uat;
    if (typo::beginTabItem("Aircraft")) { aircraftTable(t); ImGui::EndTabItem(); }
    if (typo::beginTabItem("FIS-B")) {
        for (const auto& p : t.fisb) {
            std::string head = p.title;
            if (!p.location.empty()) head += "  " + p.location;
            if (p.reportNumber) head += "  #" + std::to_string(p.reportNumber);
            if (p.cancelled) head += "  CANCELLED";
            if (!p.areas.empty()) head += "  " + std::to_string(p.areas.size()) + (p.areas.size() == 1 ? " area" : " areas");
            if (p.heard > 1) head += "  heard " + std::to_string(p.heard) + "x";
            ImGui::TextDisabled("%s", head.c_str());
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(p.text.empty() ? (p.areas.empty() ? "(no text)" : "(airspace only: its text has not been heard yet)") : p.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Separator();
        }
        ImGui::EndTabItem();
    }
    if (typo::beginTabItem("Log")) {
        for (auto it = t.framesLog.rbegin(); it != t.framesLog.rend(); ++it) {
            ImGui::TextDisabled("%.1f  %s", it->timeSec, it->kind.c_str());
            ImGui::TextWrapped("%s", it->summary.c_str());
        }
        ImGui::EndTabItem();
    }
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    tab(a);
}

void list(App& a) {
    for (const auto& ac : a.rx.uat.aircraft) {
        char b[48]; snprintf(b, sizeof b, "%06X %s", ac.icao & 0xFFFFFF, ac.callsign.empty() ? "" : ac.callsign.c_str());
        ImGui::Selectable(b, S.sel == ac.icao);
    }
}

void status(App& a) {
    const bool on = live(a);
    lamp("Data", !on ? 0 : a.rx.uat.dataValid ? 1 : 2);
    flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(on ? dect2::uatSummary(a.rx.uat).c_str() : "stopped");
    ImGui::PopFont();
    flowEnd();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "UAT 978 MHz";
    l2 = live(a) ? dect2::uatSummary(a.rx.uat) : "";
}

void synth(App& a, bool& changed) {
    int n = a.tune.synth.modeOpt[0] > 0 ? a.tune.synth.modeOpt[0] : 4;
    if (ImGui::SliderInt("Aircraft", &n, 1, 12)) { a.tune.synth.modeOpt[0] = n; changed = true; }
}

} // namespace

extern const ModeUi kUatUi;
const ModeUi kUatUi = {
    .sideTitle = "AIRCRAFT",
    .tabName = "UAT",
    .tabIcon = Ic::Compass,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
};
