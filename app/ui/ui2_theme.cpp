// The new interface: its three palettes (terminal, instrument, mono), the accent colour, the Dark / Light / High
// contrast choice and applying the theme to ImGui's style. The shared declarations are in ui2_internal.h.
// Split from ui2.cpp; the shared declarations are in ui2_internal.h.
#include "ui2_internal.h"

using namespace ui2;

namespace ui2 {

UiTheme T;

void loadTheme(int n) {
    T = UiTheme();
    if (n == 1) {
        T.bg = ImVec4(0.165f, 0.168f, 0.172f, 1); T.rail = ImVec4(0.135f, 0.138f, 0.142f, 1); T.edge = ImVec4(0.33f, 0.335f, 0.34f, 1);
        T.text = ImVec4(0.86f, 0.87f, 0.88f, 1); T.dim = ImVec4(0.66f, 0.67f, 0.68f, 1); T.bright = ImVec4(1, 1, 1, 1); T.accent = ImVec4(0.90f, 0.91f, 0.92f, 1);
        T.frame = ImVec4(0.095f, 0.098f, 0.102f, 1); T.button = ImVec4(0.235f, 0.240f, 0.247f, 1); T.plotBg = ImVec4(0.015f, 0.016f, 0.018f, 1);
        const ImVec4 s[8] = {ImVec4(0.90f, 0.91f, 0.92f, 1), ImVec4(0.55f, 0.80f, 0.62f, 1), ImVec4(0.88f, 0.68f, 0.42f, 1), ImVec4(0.62f, 0.72f, 0.92f, 1), ImVec4(0.80f, 0.58f, 0.78f, 1), ImVec4(0.55f, 0.56f, 0.58f, 1), ImVec4(0.88f, 0.84f, 0.56f, 1), ImVec4(0.35f, 0.36f, 0.38f, 1)};
        for (int k = 0; k < 8; k++) T.series[k] = s[k];
        const ImU32 t[5] = {IM_COL32(120, 128, 138, 255), IM_COL32(58, 62, 68, 255), IM_COL32(84, 90, 98, 255), IM_COL32(170, 176, 184, 255), IM_COL32(225, 228, 232, 255)};
        for (int k = 0; k < 5; k++) T.tone[k] = t[k];
        T.flair = false;
    } else if (n == 2) {
        T.bg = ImVec4(0.075f, 0.075f, 0.075f, 1); T.rail = ImVec4(0.055f, 0.055f, 0.055f, 1); T.edge = ImVec4(0.22f, 0.22f, 0.22f, 1);
        T.text = ImVec4(0.84f, 0.84f, 0.84f, 1); T.dim = ImVec4(0.50f, 0.50f, 0.50f, 1); T.bright = ImVec4(1, 1, 1, 1); T.accent = ImVec4(0.92f, 0.92f, 0.92f, 1);
        T.frame = ImVec4(0.045f, 0.045f, 0.045f, 1); T.button = ImVec4(0.115f, 0.115f, 0.115f, 1); T.plotBg = ImVec4(0.03f, 0.03f, 0.03f, 1);
        const ImVec4 s[8] = {ImVec4(0.93f, 0.93f, 0.93f, 1), ImVec4(0.62f, 0.62f, 0.62f, 1), ImVec4(0.42f, 0.42f, 0.42f, 1), ImVec4(0.80f, 0.80f, 0.80f, 1), ImVec4(0.55f, 0.55f, 0.55f, 1), ImVec4(0.35f, 0.35f, 0.35f, 1), ImVec4(0.72f, 0.72f, 0.72f, 1), ImVec4(0.28f, 0.28f, 0.28f, 1)};
        for (int k = 0; k < 8; k++) T.series[k] = s[k];
        const ImU32 t[5] = {IM_COL32(150, 150, 150, 255), IM_COL32(70, 70, 70, 255), IM_COL32(100, 100, 100, 255), IM_COL32(170, 170, 170, 255), IM_COL32(235, 235, 235, 255)};
        for (int k = 0; k < 5; k++) T.tone[k] = t[k];
        T.flair = false;
    } else {
        T.bg = ImVec4(0.045f, 0.045f, 0.044f, 1); T.rail = ImVec4(0.037f, 0.037f, 0.036f, 1); T.edge = ImVec4(0.175f, 0.172f, 0.165f, 1);
        T.text = ImVec4(0.82f, 0.81f, 0.78f, 1); T.dim = ImVec4(0.52f, 0.51f, 0.48f, 1); T.bright = ImVec4(0.96f, 0.95f, 0.92f, 1); T.accent = ImVec4(1.00f, 0.55f, 0.10f, 1);   // ember, the icon's colour
        T.frame = ImVec4(0.030f, 0.030f, 0.030f, 1); T.button = ImVec4(0.045f, 0.045f, 0.044f, 1); T.plotBg = ImVec4(0.030f, 0.030f, 0.030f, 1);
        const ImVec4 s[8] = {ImVec4(1.00f, 0.55f, 0.10f, 1), ImVec4(0.82f, 0.81f, 0.77f, 1), ImVec4(0.56f, 0.55f, 0.51f, 1), ImVec4(0.74f, 0.50f, 0.30f, 1), ImVec4(0.62f, 0.68f, 0.52f, 1), ImVec4(0.45f, 0.44f, 0.41f, 1), ImVec4(0.86f, 0.74f, 0.50f, 1), ImVec4(0.35f, 0.35f, 0.33f, 1)};
        for (int k = 0; k < 8; k++) T.series[k] = s[k];
        const ImU32 t[5] = {IM_COL32(166, 122, 46, 255), IM_COL32(86, 68, 36, 255), IM_COL32(110, 84, 36, 255), IM_COL32(168, 146, 104, 255), IM_COL32(232, 190, 112, 255)};
        for (int k = 0; k < 5; k++) T.tone[k] = t[k];
        T.flair = true;
    }
    for (int k = 0; k < 5; k++) pal::tones()[k] = T.tone[k];
}

}  // namespace ui2

// Classic or new colours and shapes. The classic style is captured once and restored when switching back.
static bool gAccentDirty = false;

static void applyUiThemeBase(App& a);

// the colour the user picked for highlights (Appearance > Accent colour); off = each palette's own
static struct { bool loaded = false, on = false; float c[3] = {0.45f, 0.72f, 0.82f}; } gUserAccent;

static void loadUserAccent() {
    if (gUserAccent.loaded) return;
    gUserAccent.loaded = true;
    plat::Prefs& p = plat::prefs();
    gUserAccent.on = p.getB("userAccentOn", false);
    for (int i = 0; i < 3; i++) { char k[24]; snprintf(k, sizeof k, "userAccent%d", i); gUserAccent.c[i] = (float)p.getD(k, gUserAccent.c[i]); }
}

static void saveUserAccent() {
    plat::Prefs& p = plat::prefs();
    p.setB("userAccentOn", gUserAccent.on);
    for (int i = 0; i < 3; i++) { char k[24]; snprintf(k, sizeof k, "userAccent%d", i); p.setD(k, gUserAccent.c[i]); }
}

namespace ui2 {

void accentMenu() {
    loadUserAccent();
    if (ImGui::Checkbox(tr("Own accent colour"), &gUserAccent.on)) { gAccentDirty = true; saveUserAccent(); }
    ImGui::SameLine();
    ImGui::BeginDisabled(!gUserAccent.on);
    if (ImGui::ColorEdit3("##useraccent", gUserAccent.c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) saveUserAccent();
    ImGui::EndDisabled();
}

// View > Font look: Current (the default), Clean, Compact. Saved now; the fonts are built at start-up, so it applies after a restart.
void typographyChoice(bool menu) {
    static const char* names[3] = {N_("Current"), N_("Clean"), N_("Compact")};
    const int cur = (int)plat::prefs().getI("uiTypography", 0);
    ImGui::TextDisabled("%s", tr("Font look (applies after a restart)"));
    for (int i = 0; i < 3; i++) {
        const bool hit = menu ? ImGui::MenuItem(tr(names[i]), nullptr, cur == i) : ImGui::RadioButton(tr(names[i]), cur == i);
        if (hit && i != cur) plat::prefs().setI("uiTypography", i);
    }
}

// View > Theme: Dark (the palettes), Light (every colour's lightness turned over, main.cpp), High contrast (below)
void themeChoice(App& a, bool menu) {
    static const char* names[3] = {N_("Dark"), N_("Light (dark on white)"), N_("High contrast (black, white, yellow)")};
    const int cur = a.highContrast ? 2 : a.lightUi ? 1 : 0;
    for (int i = 0; i < 3; i++) {
        const bool hit = menu ? ImGui::MenuItem(tr(names[i]), nullptr, cur == i) : ImGui::RadioButton(tr(names[i]), cur == i);
        if (hit && i != cur) { a.lightUi = i == 1; a.highContrast = i == 2; savePrefs(a); }
    }
}

}  // namespace ui2

// High contrast: black panes, white text and rules, yellow for what is selected or focused, 2 px outlines on everything, a 3 px tab
// overline. Dim text drawn from hard-coded greys is lifted to 7:1 on black when the frame is drawn (main.cpp, contrast.h).
static void applyHighContrast() {
    const ImVec4 black(0, 0, 0, 1), white(1, 1, 1, 1), yellow(1.f, 0.90f, 0.f, 1), grey(0.07f, 0.07f, 0.07f, 1), hov(0.22f, 0.22f, 0.22f, 1), act(0.05f, 0.05f, 0.55f, 1);
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowBorderSize = s.FrameBorderSize = s.ChildBorderSize = s.PopupBorderSize = s.TabBarBorderSize = std::max(2.f, 2.f * gUi);
    s.TabBarOverlineSize = std::max(3.f, 3.f * gUi);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = black; c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_PopupBg] = black; c[ImGuiCol_MenuBarBg] = black;
    c[ImGuiCol_Border] = white; c[ImGuiCol_Separator] = white; c[ImGuiCol_SeparatorHovered] = yellow; c[ImGuiCol_SeparatorActive] = yellow;
    c[ImGuiCol_FrameBg] = grey; c[ImGuiCol_FrameBgHovered] = hov; c[ImGuiCol_FrameBgActive] = act;
    c[ImGuiCol_Button] = grey; c[ImGuiCol_ButtonHovered] = hov; c[ImGuiCol_ButtonActive] = act;
    c[ImGuiCol_Header] = ImVec4(0.14f, 0.14f, 0.14f, 1); c[ImGuiCol_HeaderHovered] = hov; c[ImGuiCol_HeaderActive] = act;
    c[ImGuiCol_Tab] = black; c[ImGuiCol_TabHovered] = hov; c[ImGuiCol_TabSelected] = ImVec4(0.16f, 0.16f, 0.16f, 1);
    c[ImGuiCol_TabDimmed] = black; c[ImGuiCol_TabDimmedSelected] = ImVec4(0.12f, 0.12f, 0.12f, 1); c[ImGuiCol_TabSelectedOverline] = yellow; c[ImGuiCol_TabDimmedSelectedOverline] = white;
    c[ImGuiCol_SliderGrab] = yellow; c[ImGuiCol_SliderGrabActive] = white; c[ImGuiCol_CheckMark] = yellow;
    c[ImGuiCol_ScrollbarBg] = black; c[ImGuiCol_ScrollbarGrab] = ImVec4(0.8f, 0.8f, 0.8f, 1); c[ImGuiCol_ScrollbarGrabHovered] = yellow; c[ImGuiCol_ScrollbarGrabActive] = white;
    c[ImGuiCol_ResizeGrip] = ImVec4(1.f, 0.90f, 0.f, 0.6f); c[ImGuiCol_ResizeGripHovered] = yellow; c[ImGuiCol_ResizeGripActive] = white;
    c[ImGuiCol_Text] = white; c[ImGuiCol_TextDisabled] = ImVec4(0.78f, 0.78f, 0.78f, 1); c[ImGuiCol_TextSelectedBg] = ImVec4(0.05f, 0.05f, 0.55f, 1);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.14f, 0.14f, 1); c[ImGuiCol_TableBorderStrong] = white; c[ImGuiCol_TableBorderLight] = ImVec4(0.6f, 0.6f, 0.6f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_NavCursor] = yellow; c[ImGuiCol_NavWindowingHighlight] = yellow;
    pal::accentRef() = yellow;
    plt::Style& p = plt::GetStyle();
    const ImVec4 ser[8] = {yellow, ImVec4(0.f, 0.9f, 1.f, 1), ImVec4(1.f, 0.45f, 0.45f, 1), ImVec4(0.4f, 1.f, 0.4f, 1), ImVec4(1.f, 0.5f, 1.f, 1), white, ImVec4(1.f, 0.65f, 0.f, 1), ImVec4(0.6f, 0.75f, 1.f, 1)};
    for (int i = 0; i < 8; i++) p.AutoColors[i] = ser[i];
    p.PlotBg = black; p.Border = white; p.Grid = ImVec4(0.8f, 0.8f, 0.8f, 0.40f); p.Text = white;
}

static void applyUiThemeAccent(App& a);
void applyUiTheme(App& a) {
    applyUiThemeBase(a);
    applyUiThemeAccent(a);
    touch::applyMetrics(ImGui::GetStyle());   // last, and only ever raises sizes, so it is safe to repeat every frame
}
static void applyUiThemeAccent(App& a) {
    loadUserAccent();
    if (a.highContrast) { applyHighContrast(); return; }   // its yellow, not the user's accent
    if (!gUserAccent.on) return;
    const ImVec4 ac(gUserAccent.c[0], gUserAccent.c[1], gUserAccent.c[2], 1.f);
    pal::accentRef() = ac;
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_CheckMark] = ac; c[ImGuiCol_SliderGrabActive] = ac; c[ImGuiCol_TabSelectedOverline] = ac;
    c[ImGuiCol_ButtonActive] = ImVec4(ac.x * 0.7f, ac.y * 0.7f, ac.z * 0.7f, 1.f);
    c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive];
}

static void applyUiThemeBase(App& a) {
    pal::panel() = a.newUi && a.uiVariant == 7;   // round LEDs only in the Panel layout
    static ImGuiStyle classic;
    static plt::Style classicPlot;
    static ImVec4 classicAccent;
    static bool have = false;
    static int key = -1;
    static float scale = 0;
    static int epoch = -1;
    if (!have) { classic = ImGui::GetStyle(); classicPlot = plt::GetStyle(); classicAccent = pal::accentRef(); have = true; scale = gUi; }
    // the window went to a monitor with another display scale: main.cpp has just reset the style to the classic one at that scale
    // (taken again as the classic style), and the chosen theme is applied again with its sizes at the new scale
    if (gUi != scale || epoch != gStyleEpoch) { classic = ImGui::GetStyle(); classicPlot.Scale = gUi; scale = gUi; epoch = gStyleEpoch; key = -1; }
    if (gAccentDirty) { key = -1; gAccentDirty = false; }
    const int want = (a.newUi ? 1 + a.uiTheme : 0) + (a.highContrast ? 8 : 0);   // High contrast: its own key so leaving it restores the palette
    if (want == key) return;
    key = want;
    ImGui::GetStyle() = classic;
    plt::GetStyle() = classicPlot;
    pal::accentRef() = classicAccent;
    pal::dev() = a.newUi;
    if (a.newUi) loadTheme(a.uiTheme);
    pal::wfMode() = !a.newUi ? 0 : a.uiTheme == 2 ? 1 : 2;
    a.wf.palette();
    // the colour map needs a tighter range to show the noise floor as dark; the classic range comes back when switching back
    if (a.newUi && a.wf.minDb == -100 && a.wf.maxDb == -32) { a.wf.minDb = -88; a.wf.maxDb = -36; }
    else if (!a.newUi && a.wf.minDb == -88 && a.wf.maxDb == -36) { a.wf.minDb = -100; a.wf.maxDb = -32; }
    if (!a.newUi) return;
    pal::accentRef() = kAccent;

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0; s.FrameRounding = 1; s.ChildRounding = 0; s.PopupRounding = 0; s.TabRounding = 0; s.GrabRounding = 0; s.ScrollbarRounding = 0;
    s.FramePadding = ImVec2(5 * gUi, 1.5f * gUi); s.ItemSpacing = ImVec2(6 * gUi, 2.5f * gUi); s.CellPadding = ImVec2(4 * gUi, 1 * gUi);
    s.FrameBorderSize = 1; s.ChildBorderSize = 1; s.PopupBorderSize = 1; s.TabBarBorderSize = 1; s.TabBarOverlineSize = 2 * gUi;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = kBg; c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_PopupBg] = ImVec4(0.062f, 0.062f, 0.060f, 1);
    c[ImGuiCol_Border] = kEdge; c[ImGuiCol_Separator] = kEdge;
    c[ImGuiCol_FrameBg] = T.frame; c[ImGuiCol_FrameBgHovered] = ImVec4(T.frame.x + 0.05f, T.frame.y + 0.05f, T.frame.z + 0.05f, 1); c[ImGuiCol_FrameBgActive] = ImVec4(T.frame.x + 0.08f, T.frame.y + 0.08f, T.frame.z + 0.08f, 1);
    c[ImGuiCol_Button] = T.button; c[ImGuiCol_ButtonHovered] = ImVec4(T.button.x + 0.06f, T.button.y + 0.06f, T.button.z + 0.06f, 1); c[ImGuiCol_ButtonActive] = ImVec4(T.accent.x * 0.45f, T.accent.y * 0.45f, T.accent.z * 0.45f, 1);
    c[ImGuiCol_Header] = ImVec4(1, 0.95f, 0.85f, 0.06f); c[ImGuiCol_HeaderHovered] = ImVec4(1, 0.95f, 0.85f, 0.10f); c[ImGuiCol_HeaderActive] = ImVec4(T.accent.x, T.accent.y, T.accent.z, 0.30f);
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TabHovered] = ImVec4(1, 0.95f, 0.85f, 0.06f); c[ImGuiCol_TabSelected] = ImVec4(1, 0.95f, 0.85f, 0.05f);
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TabDimmedSelected] = ImVec4(1, 0.95f, 0.85f, 0.04f); c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_SliderGrab] = ImVec4(0.58f, 0.56f, 0.52f, 1); c[ImGuiCol_SliderGrabActive] = kAccent; c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_Text] = kText; c[ImGuiCol_TextDisabled] = kDim; c[ImGuiCol_MenuBarBg] = kRail;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 0.95f, 0.85f, 0.02f); c[ImGuiCol_TableHeaderBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TableBorderLight] = kEdge; c[ImGuiCol_TableBorderStrong] = kEdge;
    c[ImGuiCol_TextSelectedBg] = ImVec4(T.accent.x, T.accent.y, T.accent.z, 0.30f);
    plt::Style& p = plt::GetStyle();
    for (int i = 0; i < 8; i++) p.AutoColors[i] = T.series[i];
    p.PlotBg = T.plotBg; p.Border = kEdge; p.Grid = ImVec4(0.72f, 0.72f, 0.70f, 0.10f); p.Text = ImVec4(T.dim.x + 0.1f, T.dim.y + 0.1f, T.dim.z + 0.1f, 1);
}
