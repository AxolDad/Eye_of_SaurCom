// Tools > Stations: the other installs this Eye listens to (the station network). A table of sites with the state of each link, and Add / Edit /
// Remove. The sites, with their tokens, are kept in <data folder>/stations/sites.json (file mode 0600, like the watchlists); the links themselves
// are core/src/net/station_link.cpp. The file is read and the links started by the first stationsTick(), a few frames into the run, and the links
// are stopped in shutdownApp.
#include "../app.h"
#include "../platform/platform.h"
#include "dect2/net/station_link.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

using dect2::SiteConfig;
using dect2::SiteStatus;

constexpr double kSkewWarnSec = 30;   // a remote clock further off than this: bearings and times do not line up

struct State {
    bool started = false;
    std::vector<SiteConfig> sites;
    std::string note;             // the last problem (a file that could not be read or written)
    int sel = -1;
    bool editing = false;         // the form below the table is open
    int editIndex = -1;           // -1 = a new site
    char name[64] = "", host[128] = "", token[160] = "";
    int port = 8765;
    bool on = true;
    std::string formError;
    bool removeAsk = false;
};
State S;

std::string path() { return plat::dataDir() + "/stations/sites.json"; }

void persist() {
    std::string err;
    if (dect2::saveSites(path(), S.sites, err)) S.note.clear();
    else S.note = std::string(tr("The stations file could not be saved:")) + " " + err;
    dect2::globalLinks().configure(S.sites);   // never blocks
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

void openForm(int index) {
    S.editing = true;
    S.editIndex = index;
    S.formError.clear();
    SiteConfig c;
    if (index >= 0 && index < (int)S.sites.size()) c = S.sites[(size_t)index];
    snprintf(S.name, sizeof S.name, "%s", c.name.c_str());
    snprintf(S.host, sizeof S.host, "%s", c.host.c_str());
    snprintf(S.token, sizeof S.token, "%s", c.token.c_str());
    S.port = c.port;
    S.on = c.on;
}

void closeForm() {
    S.editing = false;
    memset(S.token, 0, sizeof S.token);   // the secret does not stay in the form buffer
}

const char* stateWord(int s) {
    switch (s) {
    case 1: return tr("Connecting");
    case 2: return tr("Live");
    case 3: return tr("Retrying");
    default: return tr("Off");
    }
}

ImVec4 stateColour(int s) {
    switch (s) {
    case 1: return ImVec4(1.f, 0.85f, 0.4f, 1.f);
    case 2: return ImVec4(0.45f, 0.9f, 0.5f, 1.f);
    case 3: return ImVec4(1.f, 0.55f, 0.35f, 1.f);
    default: return ImVec4(0.6f, 0.6f, 0.6f, 1.f);
    }
}

std::string clockOf(double t) {
    if (t <= 0) return "-";
    const time_t s = (time_t)t;
    struct tm l;
#ifdef _WIN32
    localtime_s(&l, &s);
#else
    localtime_r(&s, &l);
#endif
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", &l);
    return b;
}

bool saveForm() {
    SiteConfig c;
    c.name = trim(S.name);
    c.host = trim(S.host);
    c.token = S.token;
    c.port = S.port;
    c.on = S.on;
    if (c.name.empty()) { S.formError = tr("Give the station a name."); return false; }
    if (c.host.empty()) { S.formError = tr("Give the address of the station: a name or an IP number."); return false; }
    if (c.port < 1 || c.port > 65535) { S.formError = tr("The port is a number from 1 to 65535."); return false; }
    for (size_t i = 0; i < S.sites.size(); i++)
        if ((int)i != S.editIndex && S.sites[i].name == c.name) { S.formError = tr("Another station has this name already."); return false; }
    if (S.editIndex >= 0 && S.editIndex < (int)S.sites.size()) S.sites[(size_t)S.editIndex] = c;
    else { S.sites.push_back(c); S.sel = (int)S.sites.size() - 1; }
    persist();
    return true;
}

}  // namespace

void stationsTick(App& a) {
    (void)a;
    if (S.started) return;
    S.started = true;
    std::string err;
    if (!dect2::loadSites(path(), S.sites, err)) S.note = std::string(tr("The stations file could not be read:")) + " " + err;
    dect2::globalLinks().configure(S.sites);
}

void stationsWindow(App& a) {
    if (!a.stationsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(900 * gUi, 420 * gUi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("Stations"), &a.stationsOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }

    ImGui::TextWrapped("%s", tr("Other installs of this program whose decoded events join the Eye here, tagged with the station's name. Only decoded events cross the link: no audio and no IQ."));
    ImGui::TextColored(ImVec4(1.f, 0.72f, 0.36f, 1.f), "%s", tr("Links are not encrypted: use them on your own network or a VPN"));
    ImGui::TextDisabled("%s", tr("On each other station: Data feeds > Remote control API, turn it on, allow other computers, and copy its token here."));
    if (!S.note.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", S.note.c_str());

    const auto status = dect2::globalLinks().status();
    const double now = (double)time(nullptr);

    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    const float formH = S.editing ? 150 * gUi : 0;
    if (ImGui::BeginTable("##stations", 8, fl, ImVec2(0, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2 - formH)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn(tr("Address"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(tr("State"), ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
        ImGui::TableSetupColumn(tr("Events"), ImGuiTableColumnFlags_WidthFixed, 64 * gUi);
        ImGui::TableSetupColumn(tr("Missed"), ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
        ImGui::TableSetupColumn(tr("Last event"), ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
        ImGui::TableSetupColumn(tr("Clock skew"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn(tr("Error"), ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < S.sites.size(); i++) {
            // the live status is looked up by name (the list of the links follows the saved list, but a change takes a frame to arrive)
            SiteStatus st;
            for (const auto& p : status) if (p.first.name == S.sites[i].name) st = p.second;
            const SiteConfig& c = S.sites[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            if (ImGui::Selectable(c.name.c_str(), S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) { S.sel = (int)i; S.removeAsk = false; }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_None) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openForm((int)i);
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::Text("%s:%d", c.host.c_str(), c.port);
            ImGui::TableNextColumn();
            ImGui::TextColored(stateColour(c.on ? st.state : 0), "%s", stateWord(c.on ? st.state : 0));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)st.events);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)st.missed);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(clockOf(st.lastEventAt).c_str());
            if (ImGui::IsItemHovered() && st.lastEventAt > 0) ImGui::SetTooltip("%.0f s ago", std::max(0.0, now - st.lastEventAt));
            ImGui::TableNextColumn();
            if (st.events == 0 && st.lastEventAt <= 0) {
                ImGui::TextDisabled("-");
            } else if (std::fabs(st.clockSkewSec) > kSkewWarnSec) {
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.35f, 1.f), "%+.1f s  %s", st.clockSkewSec, tr("this station's clock is off"));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Bearings from stations whose clocks differ by more than a few seconds do not combine into a fix. Keep every station on NTP."));
            } else {
                ImGui::Text("%+.1f s", st.clockSkewSec);
            }
            ImGui::TableNextColumn();
            if (!st.lastError.empty()) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.5f, 1.f), "%s", st.lastError.c_str());   // %s: text from the network
        }
        ImGui::EndTable();
    }

    if (ImGui::Button(tr("Add..."))) openForm(-1);
    ImGui::SameLine();
    const bool have = S.sel >= 0 && S.sel < (int)S.sites.size();
    ImGui::BeginDisabled(!have);
    if (ImGui::Button(tr("Edit..."))) openForm(S.sel);
    ImGui::SameLine();
    if (S.removeAsk && have) {
        if (ImGui::Button(tr("Really remove"))) {
            S.sites.erase(S.sites.begin() + S.sel);
            S.sel = -1; S.removeAsk = false; closeForm();
            persist();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Keep"))) S.removeAsk = false;
    } else if (ImGui::Button(tr("Remove"))) {
        S.removeAsk = true;
    }
    ImGui::EndDisabled();
    if (S.sites.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("No stations yet."));
    }

    if (S.editing) {
        ImGui::Separator();
        ImGui::TextUnformatted(S.editIndex >= 0 ? tr("Edit station") : tr("Add a station"));
        ImGui::SetNextItemWidth(180 * gUi);
        ImGui::InputText(tr("Name"), S.name, sizeof S.name);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Shown on the Eye next to everything this station hears"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220 * gUi);
        ImGui::InputText(tr("Address"), S.host, sizeof S.host);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * gUi);
        ImGui::InputInt(tr("Port"), &S.port, 0, 0);
        ImGui::SetNextItemWidth(320 * gUi);
        ImGui::InputText(tr("Token"), S.token, sizeof S.token, ImGuiInputTextFlags_Password);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The token of the remote control API on that station (at least 16 characters). It stays in the stations file in your data folder."));
        ImGui::SameLine();
        ImGui::Checkbox(tr("On"), &S.on);
        if (!S.formError.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", S.formError.c_str());
        if (ImGui::Button(tr("Save"))) { if (saveForm()) closeForm(); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) closeForm();
    }
    ImGui::End();
}
