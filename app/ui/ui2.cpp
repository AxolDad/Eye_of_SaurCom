// The new interface, in a plain developer-tool style: flat, dense and monochrome. A text list of modes with keyboard shortcuts
// on the left, square bordered panes and hairlines instead of cards, one muted accent. It reuses every panel of the classic
// interface (main.cpp). This file holds the entry point drawShell2() and the Sidebar / Scope / Tiles layouts; the others
// are in ui2_faces.cpp, the shell in ui2_shell.cpp, the theme in ui2_theme.cpp.
#include "ui2_internal.h"
#include "dect2/dsp/baseband_filter.h"

using namespace ui2;

namespace ui2 {

// ---------------------------------------------------------------- variant 0: sidebar (the parent)
void bodyParent(App& a, float bodyH, ImVec2 disp) {
    // a narrow window (a laptop at 125 or 150 %): the side panel and the list on the right give up width before the spectrum does
    float sideW = std::floor(std::max(244.f * gUi, std::min(296.f * gUi, disp.x * 0.24f)));
    if (a.splitSide > 0) sideW = std::floor(clampPx(a.splitSide * gUi, 170 * gUi, disp.x * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kRail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 6 * gUi));
    ImGui::BeginChild("##side", ImVec2(sideW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    sidebar(a);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    {
        const ImVec2 p = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - bodyH), ImVec2(p.x, p.y), u32(kEdge));
    }
    ImGui::SameLine(0, 0);
    splitter("##splitSide", true, 5 * gUi, bodyH, a.splitSide, sideW, 170 * gUi, disp.x * 0.5f, 1, a);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    // scrolls only when even the smallest panes do not fit (a 1366 x 768 screen at 150 %)
    ImGui::BeginChild("##content", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const bool tx = txViewFlag();
    if (!tx && sourceOptionsOpen(a)) {
        beginPane("##srcopt", ImVec2(0, 0), true);
        sourceOptions(a);
        endPane();
    }
    if (!tx) {
        beginPane("##status", ImVec2(0, 0), true);
        statusBar(a);
        endPane();
    }
    // the analysis row gives up height (down to 150) before the tabs above it go below 240; the list on the right narrows with the window
    const float availH = ImGui::GetContentRegionAvail().y;
    float logH = (a.atsc3Mode || tx) ? 0 : std::max(144.f * gUi, std::min(215.f * gUi, availH - gap - 240.f * gUi));
    if (!a.atsc3Mode && !tx && a.splitBottom > 0) logH = clampPx(a.splitBottom * gUi, 90 * gUi, availH - gap - 150 * gUi);
    float rightW = std::floor(std::max(236.f * gUi, std::min(330.f * gUi, ImGui::GetContentRegionAvail().x * 0.30f)));
    if (a.splitRight > 0) rightW = std::floor(clampPx(a.splitRight * gUi, 150 * gUi, ImGui::GetContentRegionAvail().x - gap - 260 * gUi));
    const float mainH = std::max(186.f * gUi, availH - (logH > 0 ? logH + gap : 0));
    const float mainW = ImGui::GetContentRegionAvail().x - rightW - gap;
    beginPane("##main2", ImVec2(mainW, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    ImGui::SameLine(0, 0);
    splitter("##splitRight", true, gap, mainH, a.splitRight, rightW, 150 * gUi, mainW + rightW - 260 * gUi, -1, a);
    ImGui::SameLine(0, 0);
    beginPane("##right2", ImVec2(0, mainH));
    tgRect(a, TgRight);
    rightPanel(a);
    endPane();
    if (!a.atsc3Mode && !tx) {
        {   // the bar between the row above and the lower row, in the gap that is there anyway
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - gap);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
            splitter("##splitBottom", false, gap, ImGui::GetContentRegionAvail().x, a.splitBottom, logH, 90 * gUi, availH - gap - 150 * gUi, -1, a);
            ImGui::PopStyleVar();
        }
        beginPane("##const2", ImVec2(0, std::max(logH, ImGui::GetContentRegionAvail().y)));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void ribbonPane(App& a) {
    beginPane("##ribbon", ImVec2(0, 0), true);
    a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("MODE")); ImGui::SameLine(0, 6 * gUi); modeCombo(a, 130 * gUi);
    a.tgMax[TgSwitch] = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    ImGui::SameLine(0, 18 * gUi);
    displayControls(a, false);
    toolbarParts(a, TbSource | TbTuner | TbGain | TbDecoder, false);
    if (a.devices[a.devIdx].isRadio()) { ImGui::SameLine(0, 14 * gUi); radioSettingsUi(a, false); }
    if (sourceOptionsOpen(a)) sourceOptions(a);
    endPane();
}

// ---------------------------------------------------------------- variant 1: scope (the picture first: a control ribbon, a huge spectrum and waterfall)
void bodyScope(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##scope", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ribbonPane(a);
    beginPane("##status", ImVec2(0, 0), true);
    statusBar(a);
    endPane();
    const float availH = ImGui::GetContentRegionAvail().y, availW = ImGui::GetContentRegionAvail().x;
    float botH = (a.atsc3Mode ? 0 : 150) * gUi, dockW = 310 * gUi;
    if (!a.atsc3Mode && a.splitBot2 > 0) botH = clampPx(a.splitBot2 * gUi, 90 * gUi, availH - gap - 150 * gUi);
    if (a.splitDock > 0) dockW = clampPx(a.splitDock * gUi, 150 * gUi, availW - gap - 260 * gUi);
    const float mainH = availH - (botH > 0 ? botH + gap : 0);
    const float mainW = availW - dockW - gap;
    beginPane("##main2", ImVec2(mainW, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    ImGui::SameLine(0, 0);
    splitter("##splitDock", true, gap, mainH, a.splitDock, dockW, 150 * gUi, mainW + dockW - 260 * gUi, -1, a);
    ImGui::SameLine(0, 0);
    beginPane("##right2", ImVec2(0, mainH));
    tgRect(a, TgRight);
    rightPanel(a);
    endPane();
    if (!a.atsc3Mode) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - gap);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
        splitter("##splitBot2", false, gap, availW, a.splitBot2, botH, 90 * gUi, availH - gap - 150 * gUi, -1, a);
        ImGui::PopStyleVar();
        beginPane("##const2", ImVec2(0, 0));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void bodyTiles(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##tiles", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    if (sourceOptionsOpen(a)) {
        beginPane("##srcopt", ImVec2(0, 0), true);
        sourceOptions(a);
        endPane();
    }
    beginPane("##status", ImVec2(0, 0), true);
    statusBar(a);
    endPane();
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float row1 = std::floor(H * 0.60f) - gap;
    const float wA = std::floor(W * 0.46f), wB = std::floor(W * 0.24f), wC = W - wA - wB - 2 * gap;
    beginTile("##t_scope", N_("SPECTRUM / WATERFALL"), ImVec2(wA, row1));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    ImGui::SameLine();
    beginTile("##t_list", listTitle(a, true), ImVec2(wB, row1));
    tgRect(a, TgRight);
    rightPanel(a);
    endPane();
    ImGui::SameLine();
    beginTile("##t_ctl", N_("CONTROLS"), ImVec2(wC, row1));
    tgRect(a, TgToolbar);
    ImGui::TextDisabled("%s", tr("source")); toolbarParts(a, TbSource, true);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("gain")); toolbarParts(a, TbGain, true);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("tuner")); toolbarParts(a, TbTuner, true);
    ImGui::Spacing();
    a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
    ImGui::TextDisabled("%s", tr("mode")); modeList(a);
    if (a.family == 0 || a.family == 3 || ((modeUi(a.family) && modeUi(a.family)->decoder) || *dect2::bbf::keyForFamily(a.family))) { ImGui::Spacing(); ImGui::TextDisabled("%s", tr("decoder")); toolbarParts(a, TbDecoder, true); }
    endPane();
    const float row2 = ImGui::GetContentRegionAvail().y;
    const float wD = std::floor(W * 0.70f);
    if (!a.atsc3Mode) {
        beginTile("##t_plots", N_("ANALYSIS"), ImVec2(wD, row2));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
        ImGui::SameLine();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    beginTile("##t_log", N_("LOG"), ImVec2(0, row2));
    ImGui::BeginChild("##logc", ImVec2(0, 0));
    size_t n = 0;
    const std::vector<std::string> lines = a.engine.logSnapshot(n);
    for (const std::string& l : lines) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", l.c_str()); ImGui::PopTextWrapPos(); }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    endPane();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

}  // namespace ui2

void drawShell2(App& a, ImVec2 disp) {
    {   // keyboard: Alt+1 .. Alt+9 pick a mode (a bare digit is too easy to hit and restarts the radio), Space starts and stops (not while typing)
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && !ImGui::IsAnyItemActive() && !io.KeyCtrl && !io.KeySuper) {
            if (io.KeyAlt && !gFreqHovered) for (int k = 0; k < 9; k++) if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + k), false)) {
                int n = 0;
                for (int g = 0; g < kNumGroups; g++) for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g && n++ == k) selectMode(a, kModes[i].family, kModes[i].subId);
            }
            // not in the TX view: there Space keys the transmitter (tx_panel.cpp), and must not also start or stop the receiver
            if (!io.KeyAlt && !txViewFlag() && ImGui::IsKeyPressed(ImGuiKey_Space, false)) { if (a.engine.running()) a.engine.stop(); else startReceiver(a); }
        }
    }
    const float menuH = menuBar(a);
    {   // the waterfall range is set automatically once, two seconds after the receiver starts
        static bool wasRunning = false, done = false;
        static double t0 = 0;
        const bool run = a.engine.running();
        if (run && !wasRunning) { t0 = ImGui::GetTime(); done = false; }
        wasRunning = run;
        if (run && !done && ImGui::GetTime() - t0 > 2.5) { autoRange(a); done = true; }
    }
    pal::panel() = a.uiVariant == 7;
    const bool face = a.uiVariant == 3 || a.uiVariant == 7;
    ImGui::SetNextWindowPos(ImVec2(0, menuH));
    ImGui::SetNextWindowSize(ImVec2(disp.x, disp.y - menuH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (face) ImGui::PushStyleColor(ImGuiCol_WindowBg, a.uiVariant == 7 ? kPanelChassis : kChassis);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    if (face) ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(6 * gUi + ImGui::GetFrameHeight() * 4.8f, ImGui::GetFrameHeight() + 8 * gUi));
    ImGui::SetCursorPos(ImVec2(6 * gUi, 4 * gUi));
    txViewSwitch(a);   // RX / TX, top left
    ImGui::SetCursorPosX(0);
    const float sbH = ImGui::GetFrameHeight() + 8 * gUi;
    if (hamViewFlag()) {}   // the Ham view has no top bar of its own: the receiver keeps its
    else if (txViewFlag()) txTopBar(a, disp);   // the transmit view has its own top bar (the same frequency digits) and no side list
    else if (!face) topBar(a, disp);
    const float bodyH = ImGui::GetContentRegionAvail().y - sbH;
    if (hamViewFlag()) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * gUi, 6 * gUi));
        ImGui::BeginChild("##hambody", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        hamPanel(a);
        ImGui::EndChild();
    } else if (txViewFlag()) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * gUi, 6 * gUi));
        ImGui::BeginChild("##txbody", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        txPanel(a);
        ImGui::EndChild();
    } else switch (a.uiVariant) {
    case 1: bodyScope(a, bodyH, disp); break;
    case 2: bodyTiles(a, bodyH, disp); break;
    case 3: bodyFace(a, bodyH, disp); break;
    case 4: bodyCinema(a, bodyH, disp); break;
    case 5: bodyMeters(a, bodyH, disp); break;
    case 6: bodyDock(a, bodyH, disp); break;
    case 7: bodyPanel(a, bodyH, disp); break;
    default: bodyParent(a, bodyH, disp); break;
    }
    bottomBar(a, disp);
    ImGui::End();
}
