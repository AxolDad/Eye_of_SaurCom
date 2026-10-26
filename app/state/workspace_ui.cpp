// The File menu part of workspaces (save, load, delete) and the two dialogs: the save name and "Restore last session?".
// The logic is in workspace.cpp. English text only.
#include "workspace.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct SaveDialog {
    bool request = false;
    char name[64] = {};
};
SaveDialog gSave;
std::string gDel;   // the workspace File > Delete workspace asks about
bool gDelRequest = false;
bool gRestoreAsked = false;

std::string trimmed(const char* s) {
    std::string t = s;
    const size_t b = t.find_first_not_of(" \t");
    if (b == std::string::npos) return std::string();
    const size_t e = t.find_last_not_of(" \t");
    return t.substr(b, e - b + 1);
}

std::string nextDefaultName(const std::vector<Workspace>& list) {
    for (int n = (int)list.size() + 1; n < 1000; n++) {
        const std::string cand = "Workspace " + std::to_string(n);
        bool used = false;
        for (const Workspace& w : list) used = used || w.name == cand;
        if (!used) return cand;
    }
    return "Workspace";
}

void saveNamed(App& a, const std::string& name) {
    std::vector<Workspace> list = workspaceList();
    const Workspace w = workspaceCapture(a, name);
    bool replaced = false;
    for (Workspace& o : list) if (o.name == name) { o = w; replaced = true; }   // the same name is saved over
    if (!replaced) list.push_back(w);
    workspaceStore(list);
    a.engine.log(std::string(replaced ? "Workspace updated: " : "Workspace saved: ") + name);
}

}  // namespace

void workspaceMenu(App& a) {
    const bool running = a.engine.running();
    const std::vector<Workspace> list = workspaceList();
    if (ImGui::MenuItem("Save workspace...")) {
        gSave.request = true;
        snprintf(gSave.name, sizeof gSave.name, "%s", nextDefaultName(list).c_str());
    }
    if (ImGui::BeginMenu("Load workspace", !list.empty())) {
        if (running) ImGui::TextDisabled("Stop the receiver to load one");
        for (const Workspace& w : list) {
            if (ImGui::MenuItem(w.name.c_str(), nullptr, false, !running)) {
                std::string why;
                if (!workspaceApply(a, w, why)) a.engine.log("Workspace not loaded: " + why);
                else if (!why.empty()) a.engine.log("Workspace loaded with a note: " + why);
                else a.engine.log("Workspace loaded: " + w.name);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Delete workspace", !list.empty())) {
        for (const Workspace& w : list) {
            if (ImGui::MenuItem(w.name.c_str())) { gDel = w.name; gDelRequest = true; }   // asks first: it sits right next to Load
        }
        ImGui::EndMenu();
    }
}

void workspaceDialogs(App& a) {
    if (gSave.request) { ImGui::OpenPopup("Save workspace"); gSave.request = false; }
    if (ImGui::BeginPopupModal("Save workspace", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Name this workspace. It keeps the source, mode, frequency,\nsample rate, gains and open windows.");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(280.f);
        const bool enter = ImGui::InputText("##wsname", gSave.name, sizeof gSave.name, ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name = trimmed(gSave.name);
        if (name.empty()) ImGui::BeginDisabled();
        if (ImGui::Button("Save") || (enter && !name.empty())) {
            saveNamed(a, name);
            ImGui::CloseCurrentPopup();
        }
        if (name.empty()) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (gDelRequest) { ImGui::OpenPopup("Delete workspace?"); gDelRequest = false; }
    if (ImGui::BeginPopupModal("Delete workspace?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete the workspace \"%s\"? This cannot be undone.", gDel.c_str());
        if (ImGui::Button("Delete") || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            std::vector<Workspace> keep;
            for (const Workspace& o : workspaceList()) if (o.name != gDel) keep.push_back(o);
            workspaceStore(keep);
            a.engine.log("Workspace deleted: " + gDel);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Another popup (the data-sharing consent, a crash note) opened at the same level closes this one without answering it: keep asking
    // while the answer is pending, but only when no other popup is up, so the two dialogs wait for each other instead of one erasing the other.
    if (workspaceRestorePending() && !ImGui::IsPopupOpen("Restore last session?") &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        ImGui::OpenPopup("Restore last session?");
        gRestoreAsked = true;
    }
    if (ImGui::BeginPopupModal("Restore last session?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!workspaceRestorePending()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        const Workspace& w = workspaceRecovered();
        ImGui::TextUnformatted("Eye of SaurCom did not close cleanly last time.");
        ImGui::Text("Last session: %s, %.3f MHz", w.devName.empty() ? "no radio" : w.devName.c_str(), w.freqMhz);
        if (!w.panels.empty()) ImGui::Text("Windows open: %s", w.panels.c_str());
        ImGui::TextUnformatted("Restore last session? The settings come back; press Start to listen again.");
        ImGui::TextDisabled("Enter: Restore   Esc: Start fresh");
        // the keyboard answers too (the dialog opens by itself after a crash; without them a keyboard-only user cannot get past it)
        const bool keyYes = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        const bool keyNo = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (ImGui::Button("Restore") || keyYes) {
            std::string why;
            workspaceRestoreAnswer(a, true, why);
            if (!why.empty()) a.engine.log("Last session: " + why);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Start fresh") || (keyNo && !keyYes)) {
            std::string why;
            workspaceRestoreAnswer(a, false, why);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
