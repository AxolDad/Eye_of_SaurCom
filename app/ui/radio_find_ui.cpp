// "Find this radio": the window that asks the user to unplug a radio and plug it back (state/radio_watch.cpp does the watching).
#include "radio_find_ui.h"
#include "../app.h"
#include "../state/radio_watch.h"
#include "radio_name_ui.h"

void radioFindModal(App& a) {
    const char* const kId = "Find this radio###rfind";
    if (radioFindOpenRequested(a.rxId)) ImGui::OpenPopup(kId);
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (a.rxId == radioFindRx() && radioFind().active()) radioFindCancel();   // closed with Esc
        return;
    }
    using dect2::RadioFind;
    const RadioFind& f = radioFind();
    const RadioFindView& v = radioFindView();
    const double now = radioWatchNow();
    ImGui::PushTextWrapPos(420 * gUi);
    bool close = false;
    switch (f.state()) {
    case RadioFind::WaitUnplug:
        ImGui::TextUnformatted(tr("Unplug the radio you want to find…"));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Eye of SaurCom is watching the USB ports. The radios stay as they are."));
        break;
    case RadioFind::WaitReplug:
        ImGui::TextUnformatted(tr("…now plug it back in"));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("A radio was unplugged:"));
        { std::string s = "  " + (v.left.empty() ? f.left().name : v.left); ImGui::TextUnformatted(s.c_str()); }   // a name is text, never a format string
        break;
    case RadioFind::Found: {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "%s", tr("Found it:"));
        const std::string s = "  " + (v.back.empty() ? f.returned().name : v.back);
        ImGui::TextUnformatted(s.c_str());
        if (f.wrongRadio()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", tr("This is not the same radio you unplugged (a different serial or port). It is the one that came back."));
        }
        if (v.selected) ImGui::TextDisabled("%s", tr("It is chosen in the radio list."));
        else if (!v.note.empty()) { ImGui::Spacing(); ImGui::TextUnformatted(v.note.c_str()); }
        break;
    }
    case RadioFind::TimedOut:
        ImGui::TextUnformatted(tr("No radio came back within 60 seconds."));
        break;
    default:
        close = true;
        break;
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (f.active()) {
        ImGui::TextDisabled(tr("%d s left"), (int)(f.remaining(now) + 0.5));
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) { radioFindCancel(); close = true; }
    } else if (f.state() == RadioFind::Found) {
        if (v.devIdx >= 2 && ImGui::Button(tr("Name it"))) { radioNameRequest(v.devIdx); close = true; }   // the naming window opens next frame, from the toolbar
        if (v.devIdx >= 2) ImGui::SameLine();
        if (ImGui::Button(tr("Done"))) close = true;
    } else if (f.state() == RadioFind::TimedOut) {
        if (ImGui::Button(tr("Try again"))) radioFindStart(a);
        ImGui::SameLine();
        if (ImGui::Button(tr("Close"))) close = true;
    }
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
