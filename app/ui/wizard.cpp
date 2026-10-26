// The setup wizard (language, radio, location, goals).
#include "../app.h"
#include "../panels/fixit.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/updater.h"
#include "tour_logic.h"

// The first-run tour is in tour.cpp.


// ---- The setup wizard: language, radio, location, goals -> a recommended starting mode, tuned to a sensible default ----
namespace {

struct Region {
    const char* name;
    int tvFamily, tvStd;   // the family of the digital TV there (0 DVB, 1 ATSC, 4 ISDB-T), and the DVB standard (1 T2, 2 T)
    double fmMhz;          // a sensible FM broadcast frequency
    const char* tvId;      // when not null: the TV mode by id instead (China)
};
const Region kRegions[] = {
    {N_("Europe"), 0, 1, 100.0, nullptr},
    {N_("United Kingdom and Ireland"), 0, 1, 97.0, nullptr},
    {N_("North America"), 1, 0, 99.5, nullptr},
    {N_("Middle East"), 0, 1, 93.9, nullptr},
    {N_("Australia and New Zealand"), 0, 2, 101.1, nullptr},
    {N_("Japan"), 4, 0, 90.0, nullptr},
    {N_("China"), 0, 0, 100.0, "dtmb"},
    {N_("Elsewhere"), 0, 1, 100.0, nullptr},
};
constexpr int kNumRegions = (int)(sizeof kRegions / sizeof *kRegions);

struct Goal { const char* label; const char* modeId; };   // the first ticked one (in this order) picks the starting mode
const Goal kGoals[] = {
    {N_("FM / DAB radio"), "fm"},
    {N_("Television"), "tv"},
    {N_("Aircraft"), "adsb"},
    {N_("Ships"), "ais"},
    {N_("Satellites"), "meteor"},
    {N_("Amateur radio"), "hfdig"},
    {N_("Trunked and two-way radio"), "dmr"},
    {N_("Sensors and IoT"), "ism"},
    {N_("Just explore"), "specan"},
};
constexpr int kNumGoals = (int)(sizeof kGoals / sizeof *kGoals);

int pickGoal(unsigned g) { for (int i = 0; i < kNumGoals; i++) if (g & (1u << i)) return i; return kNumGoals - 1; }

// Switch to the recommended mode and tune its default.
void applyGoal(App& a, int goal, const Region& r) {
    const std::string id = kGoals[goal].modeId;
    if (id == "fm") { setFamily(a, 5); a.freqMhz = r.fmMhz; return; }
    if (id == "tv") {
        if (r.tvId) { if (const ModeTuning* mt = modeTuningById(r.tvId)) { setFamily(a, mt->stdMode - 2); a.freqMhz = mt->defMhz; return; } }
        setFamily(a, r.tvFamily);
        if (r.tvFamily == 0) a.stdMode = r.tvStd;
        return;
    }
    int n = 0;
    const SubMode* t = subModes(n);
    for (int i = 0; i < n; i++) if (id == t[i].id) { setMode(a, t[i].stdMode - 2, i + 1); a.freqMhz = t[i].defMhz; return; }
    if (const ModeTuning* mt = modeTuningById(id)) { setFamily(a, mt->stdMode - 2); a.freqMhz = mt->defMhz; }
}

std::string goalModeName(int goal, const Region& r) {
    const std::string id = kGoals[goal].modeId;
    if (id == "fm") { char b[48]; snprintf(b, sizeof b, " (%.1f MHz)", r.fmMhz); return std::string(tr("FM radio")) + b; }
    if (id == "tv") return r.tvId ? modeTuningById(r.tvId)->name : r.tvFamily == 1 ? "ATSC" : r.tvFamily == 4 ? "ISDB-T" : r.tvStd == 2 ? "DVB-T" : "DVB-T2";
    int n = 0;
    const SubMode* t = subModes(n);
    for (int i = 0; i < n; i++) if (id == t[i].id) return t[i].name;
    if (const ModeTuning* mt = modeTuningById(id)) return mt->name;
    return id;
}

bool parseLoc(const char* text, double& lat, double& lon) {
    const std::string s = text;
    if (s.empty()) return false;
    return dect2::fromMaidenhead(s, lat, lon) || dect2::parseCoordinates(s, lat, lon);
}

}

void setupWizardOpen(App& a) {
    a.setupOpen = true;
    a.setupStep = 0;
    a.setupRegion = (int)std::max(0L, std::min((long)kNumRegions - 1, plat::prefs().getI("setupRegion", 0)));
    a.setupGoals = (unsigned)plat::prefs().getI("setupGoals", 0);
    snprintf(a.setupLoc, sizeof a.setupLoc, "%s", plat::prefs().getS("setupLoc", "").c_str());
}

static void setupFinish(App& a, bool apply) {
    a.setupOpen = false;
    if (apply) applyGoal(a, pickGoal(a.setupGoals), kRegions[a.setupRegion]);
    plat::prefs().setB("setupDone", true);
    plat::prefs().setI("setupRegion", a.setupRegion);
    plat::prefs().setI("setupGoals", (long)a.setupGoals);
    plat::prefs().setS("setupLoc", a.setupLoc);
    double lat, lon;
    if (parseLoc(a.setupLoc, lat, lon)) { plat::prefs().setD("adsbLat", lat); plat::prefs().setD("adsbLon", lon); }
    savePrefs(a);
    plat::prefs().flush();
    if (!plat::prefs().getB(tour::prefKey(), false)) tourStart(a);   // first run: the tour comes next
}

void setupWizard(App& a, ImVec2 disp) {
    if (!a.setupOpen) return;
    const float u = gUi;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(620.f * u, disp.x - 24 * u), 0));
    ImGui::PushStyleColor(ImGuiCol_Border, pal::accent(0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::Begin("##setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    constexpr int kSteps = 5;
    ImGui::TextColored(pal::accent(), "%s  (%d / %d)", tr("Setup"), a.setupStep + 1, kSteps);
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(560 * u);
    switch (a.setupStep) {
    case 0: {
        ImGui::TextUnformatted(tr("Which language do you want Eye of SaurCom in?"));
        ImGui::Spacing();
        if (ImGui::RadioButton(tr("Same as the system"), a.uiLang.empty())) setUiLanguage(a, "");
        int n = 0;
        const i18n::Language* L = i18n::languages(n);
        for (int i = 0; i < n; i++) if (ImGui::RadioButton(L[i].name, !a.uiLang.empty() && a.uiLang == L[i].code)) setUiLanguage(a, L[i].code);
        break;
    }
    case 1: {
        ImGui::TextUnformatted(tr("Which radio do you have?"));
        ImGui::Spacing();
        int radios = 0;
        for (int i = 0; i < (int)a.devices.size(); i++) {
            if (!a.devices[i].isRadio()) continue;
            radios++;
            if (ImGui::RadioButton((a.devices[i].name + "##r" + std::to_string(i)).c_str(), a.devIdx == i)) a.devIdx = i;
        }
        if (!radios) {
            ImGui::TextColored(pal::warnAmber(), "%s", tr("No radio found"));
            if (const fixit::Entry* e = fixit::byId("no_radio")) {
                ImGui::TextUnformatted(tr(e->why));
                int k = 1;
                for (const char* st : fixit::stepsFor(*e, fixit::currentOs())) { ImGui::Text("%d.", k++); ImGui::SameLine(); ImGui::TextUnformatted(tr(st)); }
                bool first = true;
                for (int b = 0; b < 3 && e->buttons[b].label; b++) {
                    if (!(e->buttons[b].os & fixit::currentOs())) continue;
                    if (!first) ImGui::SameLine();
                    first = false;
                    if (ImGui::Button(tr(e->buttons[b].label))) {
                        if (e->buttons[b].act == fixit::Act::OpenUrl) dect2::openUrl(e->buttons[b].arg);
                        else if (e->buttons[b].act == fixit::Act::CopyText) ImGui::SetClipboardText(e->buttons[b].arg);
                    }
                }
            }
            ImGui::Spacing();
            ImGui::TextUnformatted(tr("You can go on without one: the synthetic signal and IQ files work with no hardware."));
            ImGui::TextUnformatted(tr("A radio on the network is not found by looking: add it by its address."));
        }
        ImGui::Spacing();
        if (ImGui::Button(tr("Look again"))) refreshDevices(a);
        // a radio on the network is never found by looking: it is added by its address (a PlutoSDR by IP, rtl_tcp, SpyServer, SDR++ server, ...)
        ImGui::SameLine();
        if (ImGui::Button(tr("Add a network radio..."))) { a.netAddOpen = true; a.netAddErr.clear(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A radio on another computer or on the network: a PlutoSDR by IP, an rtl_tcp server, a SpyServer, an SDR++ server, SoapyRemote, a Hermes Lite 2 or a Red Pitaya"));
        break;
    }
    case 2: {
        ImGui::TextUnformatted(tr("Where are you? This picks the right TV standard and FM band."));
        ImGui::Spacing();
        ImGui::SetNextItemWidth(260 * u);
        if (ImGui::BeginCombo("##region", tr(kRegions[a.setupRegion].name))) {
            for (int i = 0; i < kNumRegions; i++) if (ImGui::Selectable(tr(kRegions[i].name), i == a.setupRegion)) a.setupRegion = i;
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Optional: your position, as a Maidenhead locator (KO24pq) or coordinates (54.68, 25.28). It is kept on this computer and used for maps and satellite passes."));
        ImGui::SetNextItemWidth(260 * u);
        ImGui::InputText("##loc", a.setupLoc, sizeof a.setupLoc);
        double lat, lon;
        if (a.setupLoc[0]) {
            if (parseLoc(a.setupLoc, lat, lon)) ImGui::TextDisabled("%.3f, %.3f  (%s)", lat, lon, dect2::toMaidenhead(lat, lon, 6).c_str());
            else ImGui::TextColored(pal::warnAmber(), "%s", tr("Not a locator or coordinates"));
        }
        break;
    }
    case 3: {
        ImGui::TextUnformatted(tr("What do you want to receive? Tick anything that interests you."));
        ImGui::Spacing();
        for (int i = 0; i < kNumGoals; i++) {
            bool on = (a.setupGoals >> i) & 1u;
            if (ImGui::Checkbox(tr(kGoals[i].label), &on)) a.setupGoals = on ? (a.setupGoals | (1u << i)) : (a.setupGoals & ~(1u << i));
        }
        break;
    }
    default: {
        const int g = pickGoal(a.setupGoals);
        ImGui::TextUnformatted(tr("All set. Eye of SaurCom will start here:"));
        ImGui::Spacing();
        ImGui::TextColored(pal::accent(), "%s", goalModeName(g, kRegions[a.setupRegion]).c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("You can change the mode any time, and run this setup again from Help > Setup wizard."));
        break;
    }
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Separator();
    if (a.setupStep > 0 && ImGui::Button(tr("Back"), ImVec2(70 * u, 0))) a.setupStep--;
    if (a.setupStep > 0) ImGui::SameLine();
    const bool last = a.setupStep == kSteps - 1;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.34f, 0.40f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.45f, 0.53f, 1));
    if (ImGui::Button(tr(last ? "Finish" : "Next"), ImVec2(90 * u, 0))) { if (last) setupFinish(a, true); else a.setupStep++; }
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0, 18 * u);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(tr("Skip setup"))) setupFinish(a, false);
    ImGui::PopStyleColor();
    ImGui::End();
}
