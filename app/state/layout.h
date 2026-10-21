// Window layouts per mode (issue #242). The vendored Dear ImGui is the master branch (no docking, no multi-viewport), so panes cannot be
// docked or moved into their own OS windows; the floating windows instead remember their position, size and collapsed state, kept in one
// ini file per mode under dataDir()/layouts/. The main screen and the touch interface are unchanged (their windows use NoSavedSettings).
#pragma once
#include <cctype>
#include <string>

struct App;

// the file name for a mode key ("family-subId"): letters, digits, '-' and '_' kept, anything else becomes '_'
inline std::string layoutFileName(int family, const std::string& subId) {
    std::string s = std::to_string(family);
    if (!subId.empty()) { s += '-'; for (char c : subId) s += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_'; }
    return s + ".ini";
}

void layoutTick(App& a, bool enabled);   // every frame: switches the ini file when the mode changes; ImGui saves it itself when dirty
void layoutSaveNow();                    // at exit
void layoutReset();                      // View > Reset layout: forgets every saved position (all modes)
