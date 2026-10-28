// The Span bar above the spectrum and the waterfall: narrower and wider (buttons, [ and ]), the span and the resolution (type a width
// such as "50k" or "1.2M"), preset chips, fit to a signal and the right-click items. Wider than the radio captures restarts the radio at
// the smallest rate that holds the span (chooseSampleRate); narrower makes the spectrum FFT larger while its size is Auto, so a narrow
// view is sharper and not only magnified. The view itself stays in plots.cpp (one shared frequency axis, specFreqViewSet / Get).
// The pure parts (parsing, the rate and FFT choices, the occupied width) are core/src/dsp/span_view.cpp.
#include "../app.h"
#include "dect2/dsp/span_view.h"
#include "dect2/dsp/spectrum.h"
#include "dect2/engine/rate_choice.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double kMinSpanHz = 1e3;   // the narrowest view (the Zoom slider's top)

struct Pending {   // a wider span waiting for the radio to come back at its new rate
    bool on = false;
    double spanHz = 0, centreMhz = 0, t0 = 0;
    int seenFrame = -1;
};
struct State {
    Pending pending;
    std::string note;      // the last thing the bar did that is worth saying ("Radio restarted at 8 Msps to show 6 MHz")
    double noteT = -1e9;
    bool sweepHint = false;    // the last request was wider than the radio: offer the sweep
    double sweepLo = 0, sweepHi = 0, sweepT = -1e9, sweepAtSpan = 0;
    // the automatic FFT size
    int fftWant = -1, fftApplied = -2, lastSpecFft = 0;
    double fftSince = 0;
    int keyFrame = -1;
};
State S;

double fsHz(const App& a) { return a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate; }

double viewSpanHz() {
    double lo = 0, hi = 0;
    specFreqViewGet(lo, hi);
    return hi > lo ? (hi - lo) * 1e6 : 0;
}
double viewCentreMhz(const App& a) {
    double lo = 0, hi = 0;
    specFreqViewGet(lo, hi);
    return hi > lo ? (lo + hi) / 2 : radioCenterMhz(a);
}

void say(App& a, const std::string& s, bool log) {
    S.note = s;
    S.noteT = ImGui::GetTime();
    if (log) a.engine.log(s);
}

// What the source can do when a wider view is asked for
struct Widen {
    bool can = false;
    std::vector<double> rates;   // what the radio delivers for this mode
    double minHz = 0, maxHz = 0;
    std::string why;             // when it cannot: one line
};
Widen widenInfo(const App& a) {
    Widen w;
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) { w.why = tr("No source is selected."); return w; }
    const DeviceInfo& d = a.devices[a.devIdx];
    if (d.kind == DeviceInfo::File) { w.why = tr("A recording plays at the rate it was made at, so it cannot show more than it holds."); return w; }
    if (d.isAudio() || d.kind == DeviceInfo::Hamlib) { w.why = tr("A sound card captures at its own fixed rate, so the view cannot be wider."); return w; }
    if (!d.isRadio()) { w.why = tr("The test signal is made at the mode's own rate, so the view cannot be wider."); return w; }
    if (a.scanner.progress().running) { w.why = tr("A scan is using the radio and sets its own rate."); return w; }
    const RateLimits L = rateLimitsOf(d);
    w.minHz = modeMinSampleRate(a);
    for (const RateEntry& e : rateEntries(L, w.minHz)) w.rates.push_back(e.getHz);
    w.maxHz = L.maxHz;
    if (w.rates.empty()) { w.why = tr("This radio's sample rates are not known yet (start it once)."); return w; }
    w.can = true;
    return w;
}
double widestHz(const App& a, const Widen& w) {
    double m = fsHz(a);
    if (w.can) for (double r : w.rates) if (w.maxHz <= 0 || r <= w.maxHz + 1) m = std::max(m, r);
    return m;
}

std::string rateText(double hz) {   // "8 Msps"
    std::string s = formatSpan(hz);
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "Hz") == 0) s.replace(s.size() - 2, 2, "sps");
    return s;
}

void setView(App& a, double spanHz, double centreMhz) {
    spanHz = std::max(kMinSpanHz, std::min(fsHz(a), spanHz));
    specFreqViewSet(a, centreMhz - spanHz / 2e6, centreMhz + spanHz / 2e6);
    savePrefs(a);
}

} // namespace

// The one way the bar, the keys, the chips, the menu and the fits change the span
void spanSetSpan(App& a, double spanHz, double centreMhz) {
    if (!(spanHz > 0)) return;
    const double fs = fsHz(a);
    S.pending.on = false;
    if (spanHz <= fs * 1.0005) { setView(a, spanHz, centreMhz); return; }
    const Widen w = widenInfo(a);
    if (!w.can) {
        setView(a, fs, centreMhz);
        say(a, std::string(tr("Showing all of it:")) + " " + formatSpan(fs) + ". " + w.why, false);
        return;
    }
    const RatePick p = rateForSpan(spanHz, w.rates, w.minHz, w.maxHz);
    if (!p.ok) { setView(a, fs, centreMhz); say(a, tr("No faster rate of this radio suits this mode."), false); return; }
    const double want = spanHz;
    if (p.clamped) {   // wider than the radio: its widest, and the sweep for the rest
        S.sweepHint = true; S.sweepLo = centreMhz - want / 2e6; S.sweepHi = centreMhz + want / 2e6; S.sweepT = ImGui::GetTime();
        S.sweepAtSpan = p.rateHz;
        spanHz = p.rateHz;
    }
    if (p.rateHz <= fs * 1.0005) {   // already at the widest
        setView(a, fs, centreMhz);
        say(a, std::string(tr("This radio shows at most")) + " " + formatSpan(fs) + ". " + tr("Wider: use the sweep."), false);
        return;
    }
    const bool running = a.engine.running();
    chooseSampleRate(a, p.rateHz);
    S.pending = Pending{true, spanHz, centreMhz, ImGui::GetTime(), -1};
    char b[200];
    if (running) snprintf(b, sizeof b, tr("Radio restarted at %s to show %s"), rateText(p.rateHz).c_str(), formatSpan(spanHz).c_str());
    else snprintf(b, sizeof b, tr("Rate set to %s: press Start to see %s"), rateText(p.rateHz).c_str(), formatSpan(spanHz).c_str());
    std::string s = b;
    if (p.clamped) s += std::string(". ") + tr("That is this radio's widest; the sweep shows more.");
    say(a, s, true);
}

void spanShowWholeBand(App& a) { setView(a, fsHz(a), radioCenterMhz(a)); }

void spanStep(App& a, bool wider) {
    const double span = viewSpanHz() > 0 ? viewSpanHz() : fsHz(a);
    spanSetSpan(a, wider ? span * 2 : std::max(kMinSpanHz, span / 2), viewCentreMhz(a));
}

// Fit the view to the signal at mhz: its occupied width (walked out from the peak to the floor + 6 dB) and a margin, so the sidebands show
void spanFitSignal(App& a, double mhz, double mhzPerPx) {
    const size_t nb = a.smooth.size();
    const double fs = fsHz(a);
    if (nb < 16 || fs <= 0) { say(a, tr("No spectrum yet: start the receiver first."), false); return; }
    const double binHz = fs / (double)nb, x0 = radioCenterMhz(a) - fs / 2e6;
    const int bin = (int)std::floor((mhz - x0) * 1e6 / binHz);
    // the peak within about 6 pixels of the click; dips shorter than 2 kHz (an AM carrier to its sidebands) count as the signal
    const double pxHz = mhzPerPx > 0 ? mhzPerPx * 1e6 : viewSpanHz() > 0 ? viewSpanHz() / 1000 : binHz;
    const int search = std::max(2, (int)std::lround(6 * pxHz / binHz));
    const int gap = std::max(3, (int)std::lround(2e3 / binHz));
    const OccupiedBins o = occupiedAround(a.smooth.data(), (int)nb, bin, search, gap);
    if (!o.valid) { say(a, tr("No signal above the noise there to fit."), false); return; }
    const double occHz = (o.hi - o.lo + 1) * binHz;
    const double centre = x0 + (o.lo + o.hi + 1) * 0.5 * binHz / 1e6;
    char b[160];
    snprintf(b, sizeof b, tr("Fitted to a signal about %s wide"), formatSpan(occHz).c_str());
    say(a, b, false);
    spanSetSpan(a, fitSpanHz(occHz), centre);   // (a fit wider than the capture says that it restarted the radio instead)
}

// The items the spectrum's right-click menu adds (plots.cpp)
void spanContextItems(App& a, double mhz) {
    if (ImGui::MenuItem(trw("Fit to this signal"), nullptr, false, !a.smooth.empty())) spanFitSignal(a, mhz, 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom to the signal under the mouse with room for its sidebands (also Ctrl+double-click)"));
    if (ImGui::MenuItem(trw("Show the whole band"))) spanShowWholeBand(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Show everything the radio captures at its current rate"));
    if (ImGui::MenuItem(trw("Wider ×2"), "]")) spanStep(a, true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Show twice the width; past what the radio captures, it restarts at a higher rate"));
    if (ImGui::MenuItem(trw("Narrower ×2"), "[")) spanStep(a, false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Show half the width, sharper"));
}

// While the FFT size is Auto: the size follows the view, so a narrow view keeps about 1000 bins or more across it. Applied once the view
// has rested for a moment (dragging the zoom or turning the wheel would otherwise rebuild the transform every frame).
static void autoFft(App& a) {
    if (a.specFft != 0) { S.lastSpecFft = a.specFft; S.fftApplied = -2; S.fftWant = -1; return; }
    if (S.lastSpecFft != 0) { S.lastSpecFft = 0; S.fftApplied = -2; }   // back to Auto from a size picked by hand: the engine's own Auto (combo) is in force
    if (!a.engine.running()) return;
    const double fs = fsHz(a), span = viewSpanHz();
    if (fs <= 0 || span <= 0) return;
    const int n = fftForSpan(span, fs);
    const int pass = n == (int)SpectrumAnalyzer::autoSize(fs) ? 0 : n;   // 0: the engine's own Auto, which also follows a new rate
    const double now = ImGui::GetTime();
    if (pass != S.fftWant) { S.fftWant = pass; S.fftSince = now; }
    if (S.fftApplied == -2) S.fftApplied = 0;   // the engine is on its own Auto
    if (S.fftWant != S.fftApplied && now - S.fftSince > 0.4 && !ImGui::IsMouseDown(0)) {
        a.engine.setSpectrumFft(S.fftWant);
        S.fftApplied = S.fftWant;
    }
}

static void pendingTick(App& a) {
    Pending& p = S.pending;
    if (!p.on) return;
    if (ImGui::GetTime() - p.t0 > 10) { p.on = false; return; }   // the radio did not come back at that rate: leave the view alone
    if (fsHz(a) < p.spanHz * 0.999) { p.seenFrame = -1; return; }
    // the axis resets to the whole band in the frame the rate changes (setupFreqAxis); the span is set a couple of frames later
    if (p.seenFrame < 0) { p.seenFrame = ImGui::GetFrameCount(); return; }
    if (ImGui::GetFrameCount() < p.seenFrame + 2) return;
    p.on = false;
    setView(a, p.spanHz, p.centreMhz);
}

float spanBarHeight() { return std::floor(ImGui::GetFrameHeight() + 4 * gUi); }

// The bar itself: [−] [2.4 MHz · 293 Hz/bin] [+]  10k 50k 200k 1M 2.4M Max   (note or "Wider: sweep…")
void spanBar(App& a, ImVec2 pos, float width) {
    pendingTick(a);
    autoFft(a);
    const double fs = fsHz(a);
    const double span = viewSpanHz() > 0 ? viewSpanHz() : fs;
    const Widen w = widenInfo(a);
    const double widest = widestHz(a, w);
    ImGuiIO& io = ImGui::GetIO();
    // [ narrower, ] wider: only when no text field has the keyboard, once a frame even if the panel is drawn twice
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && S.keyFrame != ImGui::GetFrameCount()) {
        S.keyFrame = ImGui::GetFrameCount();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) spanStep(a, false);
        else if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) spanStep(a, true);
    }
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    const float u = gUi, gap = 4 * u, h = ImGui::GetFrameHeight();
    const float y = pos.y + std::floor((spanBarHeight() - h) * 0.5f);
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + spanBarHeight()), ImGui::GetColorU32(ImGuiCol_FrameBg, 0.35f));
    ImGui::PushID("##spanbar");
    float x = pos.x + gap;
    const float right = pos.x + width - gap;
    const float btnW = h * 1.2f;
    // the readout: as wide as its longest text, less in a narrow window
    char cur[96];
    const size_t bins = a.spec.dbfs.size() ? a.spec.dbfs.size() : a.smooth.size();
    if (bins > 0 && fs > 0) snprintf(cur, sizeof cur, "%s \xC2\xB7 %s", formatSpan(span).c_str(), formatResolution(fs / (double)bins).c_str());
    else snprintf(cur, sizeof cur, "%s", formatSpan(span).c_str());
    const float wantW = ImGui::CalcTextSize("10.0 MHz \xC2\xB7 1.22 kHz/bin").x + 2 * ImGui::GetStyle().FramePadding.x;
    const float readW = std::max(60 * u, std::min(wantW, right - x - 2 * (btnW + gap)));

    const bool atMin = span <= kMinSpanHz * 1.01;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginDisabled(atMin);
    if (ImGui::Button("\xE2\x88\x92##spannarrow", ImVec2(btnW, h))) spanStep(a, false);   // U+2212 minus
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", atMin ? tr("Already at the narrowest view (about 1 kHz)") : tr("Narrower: show half the width, with finer detail (key [)"));
    x += btnW + gap;

    static char buf[96] = "";
    static bool editing = false;
    if (!editing) snprintf(buf, sizeof buf, "%s", cur);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::SetNextItemWidth(readW);
    const bool enter = ImGui::InputText("##spanval", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    editing = ImGui::IsItemActive();
    if (enter) {
        double hz = 0;
        if (std::strcmp(buf, cur) == 0) {}   // Enter on the readout itself: nothing to do
        else if (parseSpanHz(buf, hz)) spanSetSpan(a, hz, viewCentreMhz(a));
        else say(a, tr("Type a width such as 50k, 1.2M or 200 kHz."), false);
        editing = false;
    }
    if (ImGui::IsItemHovered() && !editing) {
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(380 * u);
        ImGui::TextUnformatted(tr("Span: how much of the band the spectrum and waterfall show, and how fine one bin is."));
        ImGui::TextUnformatted(tr("Click and type a width such as 50k, 1.2M or 200 kHz, then press Enter."));
        if (w.can) { char b[160]; snprintf(b, sizeof b, tr("Wider than the radio now captures restarts it at a higher rate (up to %s)."), formatSpan(widest).c_str()); ImGui::TextUnformatted(b); }
        else ImGui::TextUnformatted(w.why.c_str());
        ImGui::TextDisabled("%s", tr("Ctrl+drag across a signal zooms to it; Ctrl+double-click fits one; right-click for more."));
        ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
    x += readW + gap;

    const bool atMax = span >= widest * 0.999;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginDisabled(atMax && !w.can);
    if (ImGui::Button("+##spanwider", ImVec2(btnW, h))) spanStep(a, true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!atMax && span * 2 > fs * 1.0005 && w.can) {
            char b[200]; snprintf(b, sizeof b, "%s", tr("Wider: show twice the width. This is more than the radio captures now, so it restarts at a higher rate (key ])."));
            ImGui::SetTooltip("%s", b);
        } else if (atMax) ImGui::SetTooltip("%s", w.can ? tr("This is the widest this radio captures. For more, use the sweep.") : w.why.c_str());
        else ImGui::SetTooltip("%s", tr("Wider: show twice the width (key ])"));
    }
    x += btnW + gap * 2;

    // "Wider: sweep…" while the last request went past the radio and the view is still at its widest (or the radio is restarting to it)
    const double now = ImGui::GetTime();
    if (S.sweepHint && (now - S.sweepT > 30 || (!S.pending.on && now - S.sweepT > 1 && span < S.sweepAtSpan * 0.99))) S.sweepHint = false;
    const bool sweepOn = S.sweepHint && w.can;
    float sweepW = 0;
    const char* sweepLb = tr("Wider: sweep\xE2\x80\xA6");
    if (sweepOn) sweepW = ImGui::CalcTextSize(sweepLb).x + 2 * ImGui::GetStyle().FramePadding.x + gap;

    // the chips, while they fit (they go before the note and after the sweep button)
    struct Chip { const char* label; double hz; };
    const Chip chips[] = {{"10k", 10e3}, {"50k", 50e3}, {"200k", 200e3}, {"1M", 1e6}, {"2.4M", 2.4e6}, {"Max", 0}};
    for (const Chip& c : chips) {
        const double hz = c.hz > 0 ? c.hz : widest;
        if (c.hz > 0 && hz > widest * 1.0005) continue;   // impossible for this radio
        const char* lb = c.hz > 0 ? c.label : tr("Max");
        const float cw = ImGui::CalcTextSize(lb).x + 2 * ImGui::GetStyle().FramePadding.x;
        if (x + cw > right - sweepW) break;   // no room: the rest are left out (the readout and the keys still do it)
        const bool on = std::fabs(span - hz) <= hz * 0.02;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.55f));
        char id[48]; snprintf(id, sizeof id, "%s##chip%s", lb, c.label);
        if (ImGui::Button(id, ImVec2(cw, h))) spanSetSpan(a, hz, c.hz > 0 ? viewCentreMhz(a) : radioCenterMhz(a));
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            char b[240];
            if (c.hz <= 0) snprintf(b, sizeof b, w.can ? tr("Max: the widest this radio captures, %s (restarts the radio if it runs slower)") : tr("Max: all of the band this source gives, %s"), formatSpan(hz).c_str());
            else if (hz > fs * 1.0005) snprintf(b, sizeof b, tr("Show %s. That is more than the radio captures now, so it restarts at a higher rate."), formatSpan(hz).c_str());
            else snprintf(b, sizeof b, tr("Show %s around the middle of the view"), formatSpan(hz).c_str());
            ImGui::SetTooltip("%s", b);
        }
        x += cw + gap;
    }
    const float noteEnd = right - sweepW;
    if (sweepOn) {
        ImGui::SetCursorScreenPos(ImVec2(right - sweepW + gap, y));
        if (ImGui::Button(sweepLb)) sweepPresetRange(a, std::max(0.001, S.sweepLo), S.sweepHi);
        if (ImGui::IsItemHovered()) {
            char b[200]; snprintf(b, sizeof b, tr("Open the sweep scanner for %.3f to %.3f MHz: it steps the radio across a range wider than it captures at once"), std::max(0.001, S.sweepLo), S.sweepHi);
            ImGui::SetTooltip("%s", b);
        }
    }
    // the note, for a few seconds, clipped to what is left of the row
    if (!S.note.empty() && now - S.noteT < 8 && x + 20 * u < noteEnd) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float ty = pos.y + (spanBarHeight() - ImGui::GetTextLineHeight()) * 0.5f;
        dl->PushClipRect(ImVec2(x + gap, pos.y), ImVec2(noteEnd, pos.y + spanBarHeight()), true);
        dl->AddText(ImVec2(x + gap, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), S.note.c_str());
        dl->PopClipRect();
        if (ImGui::IsMouseHoveringRect(ImVec2(x + gap, pos.y), ImVec2(noteEnd, pos.y + spanBarHeight()))) ImGui::SetTooltip("%s", S.note.c_str());
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(keep);
}
