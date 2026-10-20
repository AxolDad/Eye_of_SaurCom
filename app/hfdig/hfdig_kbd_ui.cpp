// HF digital, the keyboard modes view of the main tab (called by hfdig_ui.cpp): Olivia, Contestia, MFSK, THOR, DominoEX and Hellschreiber.
// The settings of the family (tones and bandwidth, or the variant), the audio frequency of the signal (found by the decoder while none is
// locked, or typed), the band buttons, the decoded text; Hellschreiber draws what it hears as a picture, lines of columns of 14 pixels.
#include "app.h"
#include "platform/platform.h"
#include "dect2/hfdig/hfdig_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace dect2;

struct State {
    bool stick = true;
    float lastMax = 0;
    KbdConfig cf[kKbdFamilies];
    bool loaded[kKbdFamilies] = {false, false, false, false, false, false};
};
State S;

struct Band { const char* name; double mhz; };
// usual dial frequencies (upper sideband); the band plans differ by region, the tooltip says so
const Band kOlivia[] = {{"80m", 3.5830}, {"40m", 7.0385}, {"20m", 14.1065}, {"15m", 21.1065}, {"10m", 28.1265}};
const Band kContestia[] = {{"80m", 3.5830}, {"40m", 7.0385}, {"20m", 14.1095}, {"15m", 21.1095}};
const Band kMfsk[] = {{"80m", 3.5815}, {"40m", 7.0385}, {"20m", 14.0785}, {"15m", 21.0785}};
const Band kThor[] = {{"80m", 3.5800}, {"40m", 7.0385}, {"20m", 14.0770}};
const Band kDomino[] = {{"80m", 3.5830}, {"40m", 7.0385}, {"20m", 14.0715}, {"15m", 21.0715}};
const Band kHell[] = {{"80m", 3.5810}, {"40m", 7.0430}, {"20m", 14.0630}};

void load(App& a, int fam) {
    if (S.loaded[fam]) return;
    S.loaded[fam] = true;
    (void)a;
    char k[48];
    KbdConfig c;
    c.family = fam;
    snprintf(k, sizeof k, "kbd%d.tones", fam); c.tones = (int)plat::prefs().getI(k, 32);
    snprintf(k, sizeof k, "kbd%d.bw", fam); c.bandwidth = (int)plat::prefs().getI(k, fam == KbdContestia ? 1000 : 1000);
    snprintf(k, sizeof k, "kbd%d.variant", fam); c.variant = (int)plat::prefs().getI(k, kbdDefaultVariant(fam));
    snprintf(k, sizeof k, "kbd%d.centre", fam); c.centreHz = plat::prefs().getD(k, 1500);
    snprintf(k, sizeof k, "kbd%d.auto", fam); c.autoCentre = plat::prefs().getB(k, true);
    c.tones = std::clamp(c.tones, 2, 256);
    c.bandwidth = std::clamp(c.bandwidth, 125, 2000);
    if (c.variant < 0 || c.variant >= std::max(1, kbdVariantCount(fam))) c.variant = kbdDefaultVariant(fam);
    c.centreHz = std::clamp(c.centreHz, 300.0, 3500.0);
    S.cf[fam] = c;
}

void save(const KbdConfig& c) {
    char k[48];
    const int fam = c.family;
    snprintf(k, sizeof k, "kbd%d.tones", fam); plat::prefs().setI(k, c.tones);
    snprintf(k, sizeof k, "kbd%d.bw", fam); plat::prefs().setI(k, c.bandwidth);
    snprintf(k, sizeof k, "kbd%d.variant", fam); plat::prefs().setI(k, c.variant);
    snprintf(k, sizeof k, "kbd%d.centre", fam); plat::prefs().setD(k, c.centreHz);
    snprintf(k, sizeof k, "kbd%d.auto", fam); plat::prefs().setB(k, c.autoCentre);
}

void bandButtons(App& a, int fam) {
    const Band* b = kOlivia; int n = (int)IM_ARRAYSIZE(kOlivia);
    if (fam == KbdContestia) { b = kContestia; n = (int)IM_ARRAYSIZE(kContestia); }
    else if (fam == KbdMfsk) { b = kMfsk; n = (int)IM_ARRAYSIZE(kMfsk); }
    else if (fam == KbdThor) { b = kThor; n = (int)IM_ARRAYSIZE(kThor); }
    else if (fam == KbdDominoex) { b = kDomino; n = (int)IM_ARRAYSIZE(kDomino); }
    else if (fam == KbdHell) { b = kHell; n = (int)IM_ARRAYSIZE(kHell); }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Band"); flowNext(6 * gUi);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - b[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        ImGui::PushID(i);
        if (ImGui::SmallButton(b[i].name)) tuneFreq(a, b[i].mhz);
        ImGui::PopID();
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s on %s: dial %.4f MHz (upper sideband), the usual place; band plans differ by region", kbdFamilyName(fam), b[i].name, b[i].mhz);
        flowNext(4 * gUi);
    }
    flowEnd();
}

void hellPicture(App& a, const HfdigKbdTelemetry& k) {
    static bool invert = false, dual = false;
    ImGui::Checkbox("Invert (black on white)", &invert); ImGui::SameLine(0, 10 * gUi);
    ImGui::Checkbox("Dual line", &dual);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every line is drawn twice, the second copy half a line later, so a character cut by the line end or a slanted picture is whole in one of them");
    ImGui::BeginChild("##hellpic", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float px = std::max(2.f, std::floor(2.5f * gUi)), gap = 6 * gUi;
    const float lineH = (dual ? 28 : 14) * px + gap + (dual ? px : 0);
    const int perLine = std::max(8, (int)(avail.x / px));
    const int lines = std::max(1, (int)(avail.y / lineH));
    const int total = k.hellCols;
    if (total == 0) {
        ImGui::TextDisabled("%s", a.engine.running() ? "no picture yet: the tone shows here as lines of dots" : "start the receiver");
        ImGui::EndChild();
        return;
    }
    // the newest columns fill the last line; lines above hold the older ones
    int nLines = std::min(lines, (total + perLine - 1) / perLine);
    int first = std::max(0, total - perLine * nLines);
    const ImU32 bg = invert ? IM_COL32(245, 245, 240, 255) : IM_COL32(18, 20, 24, 255);
    dl->AddRectFilled(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), bg);
    const int copies = dual ? 2 : 1;
    for (int cp = 0; cp < copies; cp++) {
        const int shift = cp * (perLine / 2);   // the second copy is half a line further on
        for (int c = first + shift; c < total; c++) {
            const int li = (c - first - shift) / perLine, ci = (c - first - shift) % perLine;
            if (li >= nLines) break;
            const float x = p0.x + ci * px, y = p0.y + li * lineH + cp * (14 * px + px);
            const uint8_t* col = &k.hell[(size_t)c * kKbdHellRows];
            for (int r = 0; r < kKbdHellRows; r++) {
                if (col[r] < 40) continue;
                const int v = col[r];
                const ImU32 ink = invert ? IM_COL32(235 - v * 235 / 255, 235 - v * 235 / 255, 235 - v * 235 / 255, 255)
                                         : IM_COL32(60 + v * 195 / 255, 70 + v * 185 / 255, 60 + v * 150 / 255, 255);
                dl->AddRectFilled(ImVec2(x, y + r * px), ImVec2(x + px, y + (r + 1) * px), ink);
            }
        }
    }
    ImGui::EndChild();
}

}

void hfdigKbdTab(App& a, const dect2::HfdigTelemetry& t) {
    const SubMode* sm = curSub(a);
    const int fam = sm && sm->decoders == 32 ? std::clamp(sm->sel, 0, kKbdFamilies - 1) : KbdOlivia;
    load(a, fam);
    KbdConfig c = S.cf[fam];
    const HfdigKbdTelemetry& k = t.kbd;
    HfdigKbd& dec = a.engine.hfdig().kbd();
    const bool run = a.engine.running();
    bool changed = false;

    ImGui::AlignTextToFramePadding();
    if (fam == KbdOlivia || fam == KbdContestia) {
        ImGui::TextDisabled("Tones"); ImGui::SameLine(0, 5 * gUi);
        char lbl[32];
        snprintf(lbl, sizeof lbl, "%d", c.tones);
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::BeginCombo("##kbtones", lbl)) {
            for (int v : kKbdTonesList) { snprintf(lbl, sizeof lbl, "%d", v); if (ImGui::Selectable(lbl, v == c.tones)) { c.tones = v; changed = true; } }
            ImGui::EndCombo();
        }
        flowNext(12 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Bandwidth"); ImGui::SameLine(0, 5 * gUi);
        snprintf(lbl, sizeof lbl, "%d Hz", c.bandwidth);
        ImGui::SetNextItemWidth(92 * gUi);
        if (ImGui::BeginCombo("##kbbw", lbl)) {
            for (int v : kKbdBwList) { snprintf(lbl, sizeof lbl, "%d Hz", v); if (ImGui::Selectable(lbl, v == c.bandwidth)) { c.bandwidth = v; changed = true; } }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("Mode"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(130 * gUi);
        if (ImGui::BeginCombo("##kbvar", kbdVariantName(fam, c.variant))) {
            for (int v = 0; v < kbdVariantCount(fam); v++) if (ImGui::Selectable(kbdVariantName(fam, v), v == c.variant)) { c.variant = v; changed = true; }
            ImGui::EndCombo();
        }
    }
    flowNext(12 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Audio"); ImGui::SameLine(0, 5 * gUi);
    float centre = (float)c.centreHz;
    ImGui::SetNextItemWidth(84 * gUi);
    if (ImGui::InputFloat("##kbcentre", &centre, 0, 0, "%.0f Hz")) { c.centreHz = std::clamp((double)centre, 300.0, 3500.0); changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The audio frequency of the middle of the signal, 300 .. 3500 Hz above the dial.");
    flowNext(8 * gUi);
    if (ImGui::Checkbox("Find signal", &c.autoCentre)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look for the signal in the audio while none is decoding; it moves the audio frequency above.");
    flowNext(12 * gUi);
    if (ImGui::Button("Clear")) dec.clearText();
    flowNext(6 * gUi);
    if (ImGui::Button("Copy")) ImGui::SetClipboardText(k.text.c_str());
    flowEnd();
    if (changed) { S.cf[fam] = c; save(c); }
    dec.setConfig(c);

    // status line
    const bool on = run && k.state == 1;
    lamp(kbdFamilyName(fam), !run ? 0 : (on ? 1 : 2)); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0);
    if (!run) ImGui::TextDisabled("start the receiver");
    else if (on) ImGui::Text("%s at %.0f Hz", fam == KbdOlivia || fam == KbdContestia ? "locked" : kbdVariantName(fam, k.cfg.variant), k.centreHz + k.offsetHz);
    else ImGui::TextDisabled("searching, listening at %.0f Hz", k.centreHz > 0 ? k.centreHz : c.centreHz);
    ImGui::PopFont();
    flowNext(12 * gUi);
    ImGui::TextDisabled("Bandwidth"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::Text("%.0f Hz", kbdBandwidthHz(c)); ImGui::PopFont();
    flowNext(12 * gUi);
    ImGui::TextDisabled("Level"); ImGui::SameLine(0, 5 * gUi);
    const float q = on ? std::clamp(k.snr / 30.f, 0.f, 1.f) : 0.f;
    char qt[16];
    snprintf(qt, sizeof qt, "%.0f dB", on ? k.snr : 0.f);
    gaugePill(70 * gUi, q, ImGui::ColorConvertFloat4ToU32(q > 0.5f ? pal::okGreen() : (q > 0.2f ? pal::warnAmber() : pal::badRed())), qt);
    if (fam != KbdHell) {
        flowNext(12 * gUi);
        ImGui::TextDisabled("Chars"); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::Text("%llu", (unsigned long long)k.chars); ImGui::PopFont();
        if (!k.callsign.empty()) {
            flowNext(12 * gUi);
            ImGui::TextDisabled("Callsign"); ImGui::SameLine(0, 5 * gUi);
            ImGui::PushFont(a.mono, 0); ImGui::TextColored(pal::okGreen(), "%s", k.callsign.c_str()); ImGui::PopFont();
        }
    }
    flowEnd();
    bandButtons(a, fam);

    if (fam == KbdHell) { hellPicture(a, k); return; }
    ImGui::BeginChild("##kbdtext", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0);
    ImGui::PushTextWrapPos(0);
    if (k.text.empty()) ImGui::TextDisabled("%s", run ? "no text yet" : "start the receiver");
    else ImGui::TextUnformatted(k.text.c_str(), k.text.c_str() + k.text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    const float sy = ImGui::GetScrollY(), sm2 = ImGui::GetScrollMaxY();
    if (S.stick && sy < S.lastMax - 4) S.stick = false;
    else if (!S.stick && sy >= sm2 - 4) S.stick = true;
    S.lastMax = sm2;
    if (S.stick) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}
