// The folder the desktop app keeps its data in, worked out without the app's platform layer, so the headless server finds the same files.
// Must give the same folder as plat::dataDir() (app/platform/platform_posix.cpp for Linux and Windows, platform_cocoa.mm for macOS);
// change the rules in all three together. Only computes the path: it creates nothing.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <string>

namespace dect2 {

inline std::string defaultDataDir() {
    if (const char* o = std::getenv("DECT2_DATA_DIR"); o && *o) return o;   // another folder for everything (test runs)
#if defined(_WIN32)
    if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA"); la && *la) {
        const auto u = (std::filesystem::path(la) / L"SaurCom").u8string();
        return std::string(u.begin(), u.end());
    }
    return std::string();   // the app falls back to its cache folder here; the server then looks in the current folder
#elif defined(__APPLE__)
    const char* h = std::getenv("HOME");
    return std::string(h ? h : "/tmp") + "/Library/Application Support/SaurCom";
#else
    const char* x = std::getenv("XDG_DATA_HOME");
    const char* h = std::getenv("HOME");
    return x && *x ? std::string(x) + "/saurcom" : std::string(h ? h : "/tmp") + "/.local/share/saurcom";
#endif
}

}  // namespace dect2
