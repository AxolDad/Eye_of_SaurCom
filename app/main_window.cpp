// The main window: choosing the GLFW platform on Linux, creating and sizing the window, and loading the fonts.
// Split from main.cpp; main() calls createMainWindow() and loadFonts().
// Linux picks X11 or Wayland here; on macOS and Windows the window is created as is.
#ifdef __linux__
#include <unistd.h>
#endif
#include "app.h"
#include "ui/typography.h"
#include "ui/contrast.h"
#include "dect2/source/iq_file.h"
#include "panels/gallery.h"
#include "dect2/engine/crash_report.h"
#include "platform/discord_rpc.h"
#include <chrono>
#include <cstdlib>
#if defined(__linux__)
#include <dlfcn.h>
#endif
#include "main_internal.h"

bool gWayland = false;

#if defined(__linux__)
// Linux: X11 (XWayland on a Wayland desktop) unless asked for Wayland.
// Crash report from 0.2.2 (Ubuntu 26.04, GLFW 3.4 on Wayland, "after some time"): SIGSEGV reading 0x40 in wl_proxy_get_version(NULL), called
// from GLFW's event loop (glfwPollEvents / glfwWaitEventsTimeout -> handleEvents -> the cursor timer -> setCursorImage). 0x40 is where
// libwayland keeps a proxy's version: GLFW sent a request on a NULL wl_pointer. GLFW 3.4 animates every theme cursor that has a frame delay
// on a timer while the pointer is over the window; when the seat loses its pointer (the mouse sleeps or is unplugged, a KVM switch, the
// screen locks or turns off) it destroys the wl_pointer and sets it to NULL, but neither disarms the timer nor forgets the window under the
// pointer, so the next tick calls wl_pointer_set_cursor(NULL, ...). That is inside GLFW and cannot be caught from here, so:
// - by default Eye of ZorCom asks a GLFW that has both (3.4 and later) for X11 when an X server is there (every GNOME / KDE Wayland session runs
//   XWayland); the X11 code has none of this
// - ZORCOM_DISPLAY=wayland (or --wayland, or the "displayServer" setting) keeps native Wayland; ZORCOM_DISPLAY=x11 / --x11 forces X11
// - on native Wayland ImGui does not change the cursor shape (ImGuiConfigFlags_NoMouseCursorChange): with the default cursor GLFW's timer
//   finds no cursor of Eye of ZorCom's to animate, which avoids the crash at the cost of resize / text cursors
// The functions are looked up at run time: the build may have the GLFW 3.3 headers, the library on the user's system decides.
constexpr int kGlfwPlatform = 0x00050003, kGlfwAnyPlatform = 0x00060000, kGlfwPlatformWayland = 0x00060003, kGlfwPlatformX11 = 0x00060004;
static void chooseGlfwPlatform(int argc, char** argv) {
    std::string want = plat::prefs().getS("displayServer", "auto");
    if (const char* e = getenv("ZORCOM_DISPLAY")) if (*e) want = e;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--wayland") want = "wayland";
        if (std::string(argv[i]) == "--x11") want = "x11";
    }
    auto supported = (int (*)(int))dlsym(RTLD_DEFAULT, "glfwPlatformSupported");
    if (!supported) return;   // GLFW 3.3: one platform per library
    const char* wd = getenv("WAYLAND_DISPLAY");
    const char* xd = getenv("DISPLAY");
    const bool wl = wd && *wd, x11 = xd && *xd;
    if (want == "wayland") {
        if (supported(kGlfwPlatformWayland)) glfwInitHint(kGlfwPlatform, kGlfwPlatformWayland);
    } else if ((want == "x11" || (wl && x11)) && supported(kGlfwPlatformX11)) {
        glfwInitHint(kGlfwPlatform, kGlfwPlatformX11);
        if (wl) fprintf(stderr, "display: X11 (XWayland) on a Wayland desktop; ZORCOM_DISPLAY=wayland for native Wayland\n");
    }
}
static bool glfwOnWayland() {
    if (auto getPlatform = (int (*)())dlsym(RTLD_DEFAULT, "glfwGetPlatform")) return getPlatform() == kGlfwPlatformWayland;
    auto display = (void* (*)())dlsym(RTLD_DEFAULT, "glfwGetWaylandDisplay");   // GLFW 3.3 built for Wayland
    return display && display();
}
#endif

// glfwInit (the platform on Linux), the hints, the window and its size; null when there is no window
GLFWwindow* createMainWindow(int argc, char** argv) {
#if defined(__linux__)
    chooseGlfwPlatform(argc, argv);
    if (!glfwInit()) {   // X11 asked for and no X server after all: whatever GLFW finds
        glfwInitHint(kGlfwPlatform, kGlfwAnyPlatform);
        glfwInit();
    }
    gWayland = glfwOnWayland();
    if (gWayland) fprintf(stderr, "display: native Wayland (window positions are left to the compositor; the cursor shape stays the default)\n");
#else
    glfwInit();
#endif
    gfx::windowHints();
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);   // a fullscreen main window stays up when another window or monitor is clicked (#71)
    for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--hidden") { glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); gNoOsWindows = true; }   // dev: with --shot, render without showing the window
    GLFWwindow* window = glfwCreateWindow(1500, 900, "Eye of ZorCom", nullptr, nullptr);
    if (!window) return nullptr;
#ifndef __APPLE__
    {   // the default size is in pixels: make the window as large (in points) as on a 96 dpi display, but never larger than the screen
        // (a 1366 x 768 laptop at 100 % is common: a 1500 x 900 window would hang off its bottom and right edges, hiding the status bar)
        float xs = 1, ys = 1;
        glfwGetWindowContentScale(window, &xs, &ys);
        const float s = std::max(1.f, std::max(xs, ys));
        int wx = 0, wy = 0, ww = 0, wh = 0;
        if (GLFWmonitor* mon = primaryMonitor()) glfwGetMonitorWorkarea(mon, &wx, &wy, &ww, &wh);
        int fl = 0, ft = 0, fr = 0, fb = 0;   // the frame and title bar around the window's content
        glfwGetWindowFrameSize(window, &fl, &ft, &fr, &fb);
        int w = (int)(1500 * s), h = (int)(900 * s);
        if (ww > 0 && wh > 0) { w = std::max(640, std::min(w, ww - fl - fr)); h = std::max(480, std::min(h, wh - ft - fb)); }
        if (w != 1500 || h != 900) {
            glfwSetWindowSize(window, w, h);
            if (ww > 0 && wh > 0 && !gWayland) {   // and wholly on the screen (Wayland: the compositor places it)
                int x = 0, y = 0;
                glfwGetWindowPos(window, &x, &y);
                x = std::max(wx + fl, std::min(x, wx + ww - fr - w));
                y = std::max(wy + ft, std::min(y, wy + wh - fb - h));
                glfwSetWindowPos(window, x, y);
            }
        }
    }
#endif
    if (const char* e = getenv("DECT2_WINSIZE")) { int w = 0, h = 0; if (sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) glfwSetWindowSize(window, w, h); }   // dev: try a small screen
    gWindow = window;
    return window;
}

// the UI and monospace fonts, with the CJK system fonts merged in
// (ImGui 1.92: MergeMode always merges into the font added LAST, whatever ImFontConfig::DstFont says, so each base font gets its CJK fonts before the next base font is added;
// merging into the first font after the second was added trips an assert in ImFontAtlasBuildSetupFontSpecialGlyphs in a Debug build, #104)
static void mergeCjk(ImGuiIO& io, ImFont* dst) {
    for (const auto& f : plat::cjkFontCandidates()) {
        if (access(f.c_str(), R_OK) != 0) continue;
        ImFontConfig mc;
        mc.MergeMode = true;
        mc.DstFont = dst;
        io.Fonts->AddFontFromFileTTF(f.c_str(), 13.0f, &mc);
    }
}

void loadFonts(App& app, ImGuiIO& io) {
    if (typo::loadFonts(io, [](ImGuiIO& i, ImFont* f) { mergeCjk(i, f); })) {   // Clean or Compact: Inter and JetBrains Mono, embedded
        app.ui = typo::fonts().regular; app.mono = typo::fonts().mono;
        return;
    }
    ImFont* ui = nullptr;
    app.mono = nullptr;
    for (const auto& f : plat::monoFontCandidates()) if (access(f.c_str(), R_OK) == 0 && (ui = io.Fonts->AddFontFromFileTTF(f.c_str(), 13.0f))) break; // the whole UI is monospaced
    if (!ui) ui = io.Fonts->AddFontDefault();
    // CJK (Korean/Japanese/Chinese): merge every system font that loads (Menlo has no Hangul; DRM Journaline needs it).
    mergeCjk(io, ui);
    for (const auto& f : plat::monoFontCandidates()) if (access(f.c_str(), R_OK) == 0 && (app.mono = io.Fonts->AddFontFromFileTTF(f.c_str(), 13.0f))) break;
    if (app.mono) mergeCjk(io, app.mono);
    app.ui = ui;
    if (!app.mono) app.mono = ui;
}
