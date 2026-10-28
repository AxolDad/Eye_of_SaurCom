#include "radio_name_ui.h"
#include "../app.h"
#include "../state/radio_names.h"
#include <cstring>

namespace {
int gReq = -1;               // an entry to open the window for
std::string gKey;            // the radio the window names (by key: the list may move while it is open)
std::string gShown;          // what the radio is called now, for the window
char gBuf[256];
std::string gErr;
// the inline field
std::string gFieldKey;
char gFieldBuf[256];
std::string gFieldErr;

void fill(char* buf, size_t n, const std::string& s) { snprintf(buf, n, "%s", s.c_str()); }
}  // namespace

void radioNameRequest(int deviceIdx) { gReq = deviceIdx; }

void radioNameModal(App& a) {
    const char* const kId = "Name this radio###rname";
    if (gReq >= 0) {
        const dect2::RadioRow* r = radioRow(a, gReq);
        if (r && !r->key.empty()) {
            gKey = r->key;
            gShown = dect2::radioDisplayName(dect2::RadioRow{r->dev, r->key, std::string(), r->dupIndex, r->sharedSerial});   // the model, serial and port
            fill(gBuf, sizeof gBuf, r->label);
            gErr.clear();
            ImGui::OpenPopup(kId);
        }
        gReq = -1;
    }
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextDisabled("%s", tr("This radio:"));
    ImGui::SameLine();
    ImGui::TextUnformatted(gShown.c_str());
    ImGui::Spacing();
    ImGui::SetNextItemWidth(320 * gUi);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##rnamein", gBuf, sizeof gBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("%s", tr("Up to 64 characters. Shown in place of the model and serial; kept when the radio is plugged in again."));
    if (!gErr.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s", gErr.c_str());
    ImGui::Spacing();
    bool close = false;
    if (ImGui::Button(tr("Save")) || enter) {
        std::string err;
        if (radionames::setName(gKey, gBuf, err)) close = true; else gErr = err;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear name"))) {
        std::string err;
        if (radionames::setName(gKey, std::string(), err)) close = true; else gErr = err;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) close = true;
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void radioNameField(App& a, bool vertical) {
    const dect2::RadioRow* r = radioRow(a, a.devIdx);
    if (!r || r->key.empty()) return;
    static unsigned fieldGen = 0;
    if (gFieldKey != r->key || (fieldGen != radionames::generation() && !ImGui::IsAnyItemActive())) { fieldGen = radionames::generation(); gFieldKey = r->key; fill(gFieldBuf, sizeof gFieldBuf, r->label); gFieldErr.clear(); }
    ImGui::TextDisabled("%s", tr("Name"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth((vertical ? 150.f : 220.f) * gUi);
    const bool enter = ImGui::InputTextWithHint("##rnamef", tr("your name for this radio"), gFieldBuf, sizeof gFieldBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
        std::string err;
        if (!radionames::setName(gFieldKey, gFieldBuf, err)) gFieldErr = err; else gFieldErr.clear();
        fill(gFieldBuf, sizeof gFieldBuf, dect2::cleanRadioName(gFieldBuf).substr(0, dect2::RadioLabels::kMaxLen));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Your own name for this radio, up to 64 characters. Shown in the radio list instead of the model and serial."));
    if (!gFieldErr.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s", gFieldErr.c_str());
    const std::string sw = sharedSerialWarning(a);
    if (!sw.empty()) { ImGui::PushTextWrapPos(vertical ? 0.0f : 380 * gUi); ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", sw.c_str()); ImGui::PopTextWrapPos(); }
}
