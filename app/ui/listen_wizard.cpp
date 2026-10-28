// "Get me listening": a three-step wizard for a new user. Step 1 picks a source (a connected radio, the demo signal or an IQ file),
// step 2 the place (its band plan, from core's bandplan registry), step 3 one of five things to listen to. It only picks settings and
// calls the existing tune and start code (setFamily / setMode / startReceiver); it adds no DSP of its own.
#include "../app.h"
#include "dect2/bandplan/bandplan.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/general/general_rx.h"

namespace bp = dect2::bandplan;

namespace {

constexpr int kListenSteps = 3;

bool gOpen = false;
int gStep = 0;
int gDev = 0;       // step 1: an index into a.devices
int gPlan = -1;     // step 2: an index into the plans of listenPlans()
int gPreset = 0;    // step 3: an index into kPresets
bool gDontShow = false;

// The places: the built-in band plans and the user's own (the same folder the band plan picker uses).
bp::Registry& listenPlans() {
    static bp::Registry r = [] { bp::Registry x; x.setUserDir(plat::dataDir() + "/bandplans"); return x; }();
    return r;
}

// The five things to listen to. modeId "" is the FM broadcast family (its frequency comes from the place's band plan unless given);
// otherwise the mode's own tuning gives the family and the frequency, unless mhz is given.
// demod: the General receiver's demodulator (-1 leaves it). Marine VHF is voice on channel 16 (the marine mode would start NAVTEX at 518 kHz).
struct Preset { const char* label; const char* modeId; double mhz; int demod; };
const Preset kPresets[] = {
    {N_("FM broadcast"), "", 0, -1},
    {N_("Airband (aircraft voice)"), "airband", 0, -1},
    {N_("Marine VHF (channel 16, 156.800 MHz)"), "general", 156.800, dect2::GenNfm},
    {N_("Weather radio (US and Canada, NOAA, 162.550 MHz)"), "same", 162.550, -1},   // the SAME mode: narrow FM, alerts decoded, its sound on
    {N_("Ham radio, 2 m (SSB calling, 144.200 MHz)"), "general", 144.200, dect2::GenUsb},
};
constexpr int kNumPresets = (int)(sizeof kPresets / sizeof *kPresets);

// The FM frequency of a plan (its first FM band), or 100 MHz when the plan has none.
double fmMhzOf(const bp::Plan* p) {
    if (p) if (const bp::Band* b = p->firstBand("fm")) { const double hz = b->firstCentreHz > 0 ? b->firstCentreHz : b->startHz; if (hz > 0) return hz / 1e6; }
    return 100.0;
}

// The radios, the demo signal and the IQ file: the sources a new user can pick (sound cards and network rigs are under the source list).
bool listenSource(const DeviceInfo& d) { return d.isRadio() || d.kind == DeviceInfo::Synthetic || d.kind == DeviceInfo::File; }

// Start on the place the app would pick anyway: the saved plan, else the one for the language and the system region.
int defaultPlanIndex(const App& a) {
    const auto& plans = listenPlans().plans();
    if (!a.bandPlan.empty() && a.bandPlan != "none")
        for (int i = 0; i < (int)plans.size(); i++) if (plans[i].id == a.bandPlan) return i;
    const std::string id = listenPlans().defaultFor(a.uiLang.empty() ? plat::uiLanguage() : a.uiLang, bp::Registry::localeFromEnv());
    for (int i = 0; i < (int)plans.size(); i++) if (plans[i].id == id) return i;
    return plans.empty() ? -1 : 0;
}

void rememberDontShow() {
    plat::prefs().setB("listenWizardHide", gDontShow);
    plat::prefs().flush();
}

void closeWizard() {
    gOpen = false;
    rememberDontShow();
}

// Source, place and the chosen thing to listen to: the settings, then the start (the same calls the toolbar makes).
void finishWizard(App& a) {
    if (gDev >= 0 && gDev < (int)a.devices.size()) a.devIdx = gDev;
    const auto& plans = listenPlans().plans();
    if (gPlan >= 0 && gPlan < (int)plans.size()) a.bandPlan = plans[gPlan].id;
    const Preset& s = kPresets[gPreset];
    // a recording keeps its own frequency: the preset's would be outside what it holds
    const bool file = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].kind == DeviceInfo::File;
    const double keep = a.freqMhz;
    if (s.modeId[0] == '\0') {
        setFamily(a, 5);   // FM broadcast family
        a.freqMhz = s.mhz > 0 ? s.mhz : fmMhzOf(gPlan >= 0 && gPlan < (int)plans.size() ? &plans[gPlan] : nullptr);
    } else if (const ModeTuning* mt = modeTuningById(s.modeId)) {
        setFamily(a, mt->stdMode - 2);
        a.freqMhz = s.mhz > 0 ? s.mhz : mt->defMhz;
    }
    if (file) a.freqMhz = keep;
    if (s.demod >= 0) generalSetDemod(s.demod);
    if (std::string(s.modeId) == "same") sameSetListen(true);
    savePrefs(a);
    if (a.engine.running()) a.engine.stop();   // run again: the new source and mode only take effect at a start
    a.wizStart = false;
    startReceiver(a);
    closeWizard();
}

}

void listenWizardOpen(App& a) {
    gOpen = true;
    gStep = 0;
    gDev = a.devIdx;
    gPlan = defaultPlanIndex(a);
    gPreset = 0;
    gDontShow = plat::prefs().getB("listenWizardHide", false);
}

void listenWizard(App& a, ImVec2 disp) {
    if (!gOpen) return;
    const float u = gUi;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(620.f * u, disp.x - 24 * u), 0));
    ImGui::PushStyleColor(ImGuiCol_Border, pal::accent(0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::Begin("##listen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::TextColored(pal::accent(), "%s  (%d / %d)", tr("Get me listening"), gStep + 1, kListenSteps);
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(560 * u);
    switch (gStep) {
    case 0: {
        ImGui::TextUnformatted(tr("Where does the signal come from?"));
        ImGui::Spacing();
        int radios = 0;
        for (int i = 0; i < (int)a.devices.size(); i++) {
            if (!listenSource(a.devices[i])) continue;
            if (a.devices[i].isRadio()) radios++;
            if (ImGui::RadioButton((radioShownName(a, i) + "##s" + std::to_string(i)).c_str(), gDev == i)) gDev = i;
        }
        // an IQ recording: choose it here, or the start would have nothing to play
        if (gDev >= 0 && gDev < (int)a.devices.size() && a.devices[gDev].kind == DeviceInfo::File) {
            if (ImGui::Button(tr("Choose file..."))) {
                const std::string p = openFileDialog();
                if (!p.empty()) { a.file.path = p; a.file.format = guessFormat(p); const double r = guessSampleRate(p); if (r > 0) a.file.sampleRate = r; applyIqFileInfo(a, p); }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", a.file.path.empty() ? tr("no file chosen yet") : a.file.path.c_str());
        }
        if (!radios) {
            ImGui::Spacing();
            ImGui::TextColored(pal::warnAmber(), "%s", tr("No radio found"));
            ImGui::TextUnformatted(tr("You can go on without one: the synthetic test signal and IQ recordings work with no hardware."));
            ImGui::TextUnformatted(tr("A radio on the network is not found by looking: add it by its address."));
            for (const auto& h : a.usbHints) ImGui::TextColored(pal::warnAmber(), "%s", h.c_str());   // the precise cause, when the USB check found one
        }
        ImGui::Spacing();
        if (ImGui::Button(tr("Refresh"))) {
            refreshDevices(a);
            if (gDev >= (int)a.devices.size()) gDev = 0;
            if (gDev < 0 || !a.devices[gDev].isRadio())   // a radio just plugged in is the one meant
                for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].isRadio()) { gDev = i; break; }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Add a network radio..."))) { a.netAddOpen = true; a.netAddErr.clear(); }
        break;
    }
    case 1: {
        ImGui::TextUnformatted(tr("Where are you? This picks the band plan: the TV channels, FM and airband of that country."));
        ImGui::Spacing();
        const auto& plans = listenPlans().plans();
        ImGui::SetNextItemWidth(260 * u);
        const char* cur = gPlan >= 0 && gPlan < (int)plans.size() ? plans[gPlan].name.c_str() : "";
        if (ImGui::BeginCombo("##listenplace", cur)) {
            for (int i = 0; i < (int)plans.size(); i++) if (ImGui::Selectable(plans[i].name.c_str(), i == gPlan)) gPlan = i;
            ImGui::EndCombo();
        }
        break;
    }
    default: {
        ImGui::TextUnformatted(tr("What do you want to listen to?"));
        ImGui::Spacing();
        for (int i = 0; i < kNumPresets; i++)
            if (ImGui::RadioButton(tr(kPresets[i].label), gPreset == i)) gPreset = i;
        break;
    }
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Checkbox(tr("Don't show again"), &gDontShow)) rememberDontShow();
    ImGui::Spacing();
    if (gStep > 0 && ImGui::Button(tr("Back"), ImVec2(70 * u, 0))) gStep--;
    if (gStep > 0) ImGui::SameLine();
    const bool last = gStep == kListenSteps - 1;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.34f, 0.40f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.45f, 0.53f, 1));
    const bool needFile = gDev >= 0 && gDev < (int)a.devices.size() && a.devices[gDev].kind == DeviceInfo::File && a.file.path.empty();
    ImGui::BeginDisabled(needFile);   // an IQ recording without a file cannot start
    if (ImGui::Button(tr(last ? "Start listening" : "Next"), ImVec2(130 * u, 0))) { if (last) finishWizard(a); else gStep++; }
    ImGui::EndDisabled();
    if (needFile && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Choose the IQ recording first"));
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0, 18 * u);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(tr("Cancel"))) closeWizard();
    ImGui::PopStyleColor();
    ImGui::End();
}
