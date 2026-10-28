// The Span bar above the spectrum and the waterfall: narrower and wider (buttons, [ and ]), the span and the resolution (type a width
// such as "50k" or "1.2M"), preset chips, Fit (to the signal at the tuned frequency), the "Zoom to selection" toggle and the right-click
// items. Wider than the radio captures restarts the radio at the smallest rate that holds the span (chooseSampleRate); narrower makes
// the spectrum FFT larger while its size is Auto (spanTick), so a narrow view is sharper and not only magnified, the waterfall too
// (its full-FFT history, Waterfall::lvFine). The view itself stays in plots.cpp (one shared frequency axis, specFreqViewSet / Get).
// The pure parts (parsing, the rate and FFT choices, the occupied width) are core/src/dsp/span_view.cpp.
#include "../app.h"
#include "dect2/dsp/span_view.h"
#include "dect2/dsp/spectrum.h"
#include "dect2/engine/rate_choice.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace {

constexpr double kMinSpanHz = 1e3;   // the narrowest view (the Zoom slider's top)

struct Pending {   // a wider span waiting for the radio to come back at its new rate
    bool on = false;
    double spanHz = 0, centreMhz = 0, t0 = 0, rateHz = 0;
    int seenFrame = -1;
};
struct State {   // per receiver (each has its own engine)
    Pending pending;
    std::string note;      // the last thing the bar did that is worth saying ("Radio restarted at 8 Msps to show 6 MHz")
    double noteT = -1e9;
    bool noteStrong = false;   // a restart: drawn in the text colour, before the chips
    bool sweepHint = false;    // the last request was wider than the radio: offer the sweep
    double sweepLo = 0, sweepHi = 0, sweepT = -1e9, sweepAtSpan = 0;
    // the automatic FFT size
    int fftWant = -1, fftApplied = 0, lastSpecFft = 0, lastFamily = -1;
    double fftSince = 0, barSeenT = -1e9;
    int keyFrame = -1;
    // the typed span
    char buf[96] = "";
    bool editing = false;
};
std::unordered_map<const App*, State> gStates;
State& st(const App& a) { return gStates[&a]; }
bool gZoomTool = false;

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
double listenMhz(const App& a) { return a.family == 55 ? generalListenHz(a) / 1e6 : a.freqMhz; }   // the frequency being listened to
// Where a narrower view goes: about the tuned frequency when it is in view (the signal being listened to, not the middle of the view,
// which with offset tuning is often the DC spike), else about the middle. A wider view stays about the middle.
double zoomCentreMhz(const App& a, double newSpanHz) {
    double lo = 0, hi = 0;
    specFreqViewGet(lo, hi);
    const double f = listenMhz(a);
    if (hi > lo && newSpanHz < (hi - lo) * 1e6 * 0.999 && f >= lo && f <= hi) return f;
    return viewCentreMhz(a);
}

void say(App& a, const std::string& s, bool log, bool strong = false) {
    State& S = st(a);
    S.note = s;
    S.noteT = ImGui::GetTime();
    S.noteStrong = strong;
    if (log) a.engine.log(s);
}

// What the source can do when a wider view is asked for
struct Widen {
    bool can = false;
    std::vector<double> rates;   // what the radio delivers for this mode
    double minHz = 0, maxHz = 0;
    std::string why;             // when it cannot: one line
};
std::string rateText(double hz) {   // "8 Msps"
    std::string s = formatSpan(hz);
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "Hz") == 0) s.replace(s.size() - 2, 2, "sps");
    return s;
}
Widen widenInfo(const App& a) {
    Widen w;
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) { w.why = tr("No source is selected."); return w; }
    const DeviceInfo& d = a.devices[a.devIdx];
    if (d.kind == DeviceInfo::File) { w.why = tr("A recording plays at the rate it was made at, so it cannot show more than it holds."); return w; }
    if (d.isAudio() || d.kind == DeviceInfo::Hamlib) { w.why = tr("A sound card captures at its own fixed rate, so the view cannot be wider."); return w; }
    if (!d.isRadio()) { w.why = tr("The test signal is made at the mode's own rate, so the view cannot be wider."); return w; }
    if (a.scanner.progress().running) { w.why = tr("A scan is using the radio and sets its own rate."); return w; }
    // the engine shows less than the radio captures (offset tuning brings it back to the mode's rate; multi-channel TV shows its span):
    // a faster radio would not widen the view, so the rate is left alone
    const double radio = a.engine.radioRate(), shown = a.engine.sampleRate();
    if (a.engine.running() && (a.engine.multiChannelActive() || (radio > 0 && shown > 0 && radio > shown * 1.001))) {
        char b[220];
        snprintf(b, sizeof b, tr("This mode shows only %s of the radio's %s, so a faster rate would not widen the view."), formatSpan(shown).c_str(), rateText(radio).c_str());
        w.why = b;
        return w;
    }
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

void setView(App& a, double spanHz, double centreMhz) {
    spanHz = std::max(kMinSpanHz, std::min(fsHz(a), spanHz));
    specFreqViewSet(a, centreMhz - spanHz / 2e6, centreMhz + spanHz / 2e6);
    savePrefs(a);
}

void pendingTick(App& a) {
    State& S = st(a);
    Pending& p = S.pending;
    if (!p.on) return;
    // the user zoomed or panned meanwhile: their view wins
    if (ImGui::GetIO().MouseWheel != 0 || ImGui::IsMouseDragging(0)) { p.on = false; return; }
    if (ImGui::GetTime() - p.t0 > 10) {   // the radio did not come back at that rate: say so, leave the view alone
        p.on = false;
        char b[200];
        snprintf(b, sizeof b, tr("The radio runs at %s, so the view stays at %s."), rateText(a.engine.radioRate() > 0 ? a.engine.radioRate() : fsHz(a)).c_str(), formatSpan(fsHz(a)).c_str());
        say(a, b, true);
        return;
    }
    if (fsHz(a) < p.spanHz * 0.999) { p.seenFrame = -1; return; }
    // the axis resets to the whole band in the frame the rate changes (setupFreqAxis); the span is set a couple of frames later
    if (p.seenFrame < 0) { p.seenFrame = ImGui::GetFrameCount(); return; }
    if (ImGui::GetFrameCount() < p.seenFrame + 2) return;
    p.on = false;
    setView(a, p.spanHz, p.centreMhz);
}

// While the FFT size is Auto and the Span bar is in use: the size follows the view, so a narrow view keeps about 1000 bins or more across
// it (at most 32768, a frame at most 1/20 s, 16384 while the receiver struggles to keep up). Applied once the view has rested for a moment
// (dragging the zoom or turning the wheel would otherwise rebuild the transform every frame). The bar not drawn for half a second, or
// another mode: back to the engine's own Auto.
void autoFft(App& a) {
    State& S = st(a);
    if (a.specFft != 0) { S.lastSpecFft = a.specFft; S.fftApplied = 0; S.fftWant = -1; return; }   // a size picked by hand: never touched
    if (S.lastSpecFft != 0) { S.lastSpecFft = 0; S.fftApplied = 0; }   // back to Auto in the combo, which set the engine's own Auto
    const double now = ImGui::GetTime();
    const bool modeChanged = S.lastFamily != a.family;
    S.lastFamily = a.family;
    int pass = 0;   // 0: the engine's own Auto, which also follows a new rate
    const double fs = fsHz(a), span = viewSpanHz();
    if (!modeChanged && now - S.barSeenT < 0.5 && a.engine.running() && fs > 0 && span > 0) {
        const SampleLoss l = a.engine.sampleLoss();
        const bool busy = l.live && (l.loadPct > 80 || (l.sinceCpuSec >= 0 && l.sinceCpuSec < 10));
        const int n = fftForSpan(span, fs, busy ? 16384 : kSpanAutoMaxFft);
        pass = n == (int)SpectrumAnalyzer::autoSize(fs) ? 0 : n;
    }
    if (pass != S.fftWant) { S.fftWant = pass; S.fftSince = now; }
    const bool settle = pass == 0 || (now - S.fftSince > 0.4 && !ImGui::IsMouseDown(0));   // back to Auto at once; up only once settled
    if (S.fftWant != S.fftApplied && settle) {
        a.engine.setSpectrumFft(S.fftWant);
        S.fftApplied = S.fftWant;
    }
}

} // namespace

bool& spanZoomToolOn() { return gZoomTool; }

void spanTick(App& a) {
    pendingTick(a);
    autoFft(a);
}

// The one way the bar, the keys, the chips, the menu and the fits change the span
void spanSetSpan(App& a, double spanHz, double centreMhz) {
    if (!(spanHz > 0)) return;
    State& S = st(a);
    const double fs = fsHz(a);
    S.pending.on = false;
    if (spanHz <= fs * 1.0005) { setView(a, spanHz, centreMhz); return; }
    const Widen w = widenInfo(a);
    if (!w.can) {
        setView(a, fs, centreMhz);
        char b[400];
        snprintf(b, sizeof b, tr("Showing all %s. %s"), formatSpan(fs).c_str(), w.why.c_str());
        say(a, b, false);
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
        char b[240];
        snprintf(b, sizeof b, tr("This radio shows at most %s. For more, use the sweep."), formatSpan(fs).c_str());
        say(a, b, false);
        return;
    }
    const bool running = a.engine.running();
    const double before = a.tune.sampleRate;
    chooseSampleRate(a, p.rateHz);
    const bool changed = std::fabs(a.tune.sampleRate - before) > 1;
    char b[240];
    if (!changed) {   // the rate in use already: the radio did not restart, and the view cannot be wider
        setView(a, fs, centreMhz);
        snprintf(b, sizeof b, tr("The radio already runs at %s; the view shows all %s."), rateText(a.tune.sampleRate).c_str(), formatSpan(fs).c_str());
        say(a, b, false);
        return;
    }
    S.pending = Pending{true, spanHz, centreMhz, ImGui::GetTime(), a.tune.sampleRate, -1};
    if (running) snprintf(b, sizeof b, p.clamped ? tr("Radio restarted at %s to show %s, its widest; the sweep shows more.") : tr("Radio restarted at %s to show %s"),
                          rateText(a.tune.sampleRate).c_str(), formatSpan(spanHz).c_str());
    else snprintf(b, sizeof b, tr("Rate set to %s: press Start to see %s"), rateText(a.tune.sampleRate).c_str(), formatSpan(spanHz).c_str());
    say(a, b, true, true);
}

void spanShowWholeBand(App& a) { setView(a, fsHz(a), radioCenterMhz(a)); }

void spanStep(App& a, bool wider) {
    const double span = viewSpanHz() > 0 ? viewSpanHz() : fsHz(a);
    const double next = wider ? span * 2 : std::max(kMinSpanHz, span / 2);
    spanSetSpan(a, next, zoomCentreMhz(a, next));
}

// Fit the view to the signal at mhz: its occupied width on the averaged spectrum the display shows (walked out from the peak to the
// floor + 6 dB, the DC spike left out, at most 20 x its -6 dB core but always 12 kHz) and a margin, so the sidebands show. Never wider
// than the radio captures now: a fit does not restart the radio.
void spanFitSignal(App& a, double mhz, double mhzPerPx) {
    const size_t nb = a.smooth.size();
    const double fs = fsHz(a);
    if (nb < 16 || fs <= 0) { say(a, tr("No spectrum yet: start the receiver first."), false); return; }
    const double binHz = fs / (double)nb, x0 = radioCenterMhz(a) - fs / 2e6;
    const int bin = (int)std::floor((mhz - x0) * 1e6 / binHz);
    const double pxHz = mhzPerPx > 0 ? mhzPerPx * 1e6 : viewSpanHz() > 0 ? viewSpanHz() / 1000 : binHz;
    OccupiedOpts op;
    op.searchBins = std::max(2, (int)std::lround(6 * pxHz / binHz));       // the peak within about 6 pixels of the click
    op.gapBins = std::min(8, std::max(2, (int)std::lround(500 / binHz)));  // a short dip (an AM carrier to its first sidebands) is the signal
    op.minCapBins = (int)std::lround(12e3 / binHz);
    op.dcBin = (int)nb / 2;                                                 // the radio's DC spike, at its centre
    op.dcHalf = std::max(2, (int)std::lround(300 / binHz));
    const OccupiedBins o = occupiedAround(a.smooth.data(), (int)nb, bin, op);
    if (!o.valid) { say(a, tr("No signal above the noise there to fit."), false); return; }
    const double occHz = (o.hi - o.lo + 1) * binHz;
    const double centre = x0 + (o.lo + o.hi + 1) * 0.5 * binHz / 1e6;
    char b[160];
    snprintf(b, sizeof b, tr("Fitted to a signal about %s wide"), formatSpan(occHz).c_str());
    say(a, b, false);
    setView(a, std::min(fs, fitSpanHz(occHz)), centre);
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Show half the width, sharper, about the tuned frequency"));
}

float spanBarHeight() { return std::floor(ImGui::GetFrameHeight() + 4 * gUi); }

// A small square icon button: a magnifier over a dashed box ("Zoom to selection")
static bool zoomToolButton(ImVec2 p, float s, bool on) {
    ImGui::SetCursorScreenPos(p);
    const bool click = ImGui::InvisibleButton("##spanzoomtool", ImVec2(s, s));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = on ? pal::accent(0.55f) : ImGui::GetStyleColorVec4(hov ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), ImGui::ColorConvertFloat4ToU32(bg), ImGui::GetStyle().FrameRounding);
    const ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text);
    const float t = std::max(1.f, 1.2f * gUi), m = s * 0.2f;
    // the box: dashed corners
    const ImVec2 b0(p.x + m, p.y + m), b1(p.x + s - m, p.y + s - m);
    const float c = (b1.x - b0.x) * 0.3f;
    dl->AddLine(b0, ImVec2(b0.x + c, b0.y), fg, t); dl->AddLine(b0, ImVec2(b0.x, b0.y + c), fg, t);
    dl->AddLine(ImVec2(b1.x, b0.y), ImVec2(b1.x - c, b0.y), fg, t); dl->AddLine(ImVec2(b1.x, b0.y), ImVec2(b1.x, b0.y + c), fg, t);
    dl->AddLine(ImVec2(b0.x, b1.y), ImVec2(b0.x + c, b1.y), fg, t); dl->AddLine(ImVec2(b0.x, b1.y), ImVec2(b0.x, b1.y - c), fg, t);
    // the magnifier in the middle
    const ImVec2 lc(p.x + s * 0.45f, p.y + s * 0.45f);
    const float r = s * 0.16f;
    dl->AddCircle(lc, r, fg, 0, t);
    dl->AddLine(ImVec2(lc.x + r * 0.7f, lc.y + r * 0.7f), ImVec2(lc.x + r * 1.9f, lc.y + r * 1.9f), fg, t * 1.3f);
    return click;
}

// The bar itself: [−] [2.4 MHz · 293 Hz/bin] [+] [Max] [Fit] [zoom tool]  10k 50k 200k 1M 2.4M   (note or "Wider: sweep…")
void spanBar(App& a, ImVec2 pos, float width) {
    State& S = st(a);
    S.barSeenT = ImGui::GetTime();
    const double fs = fsHz(a);
    const double span = viewSpanHz() > 0 ? viewSpanHz() : fs;
    const Widen w = widenInfo(a);
    const double widest = widestHz(a, w);
    ImGuiIO& io = ImGui::GetIO();
    // [ narrower, ] wider: only when no text field has the keyboard and nothing is being sent, once a frame even if drawn twice
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !txActive() && S.keyFrame != ImGui::GetFrameCount()) {
        S.keyFrame = ImGui::GetFrameCount();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) spanStep(a, false);
        else if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) spanStep(a, true);
    }
    if (gZoomTool && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) gZoomTool = false;
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    const float u = gUi, gap = 4 * u, h = ImGui::GetFrameHeight();
    const float y = pos.y + std::floor((spanBarHeight() - h) * 0.5f);
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + spanBarHeight()), ImGui::GetColorU32(ImGuiCol_FrameBg, 0.35f));
    ImGui::PushID("##spanbar");
    float x = pos.x + gap;
    const float right = pos.x + width - gap;
    const float btnW = h * 1.2f;
    const float fp = 2 * ImGui::GetStyle().FramePadding.x;
    // the readout: as wide as its longest text, less in a narrow window
    char cur[96];
    const size_t bins = a.spec.dbfs.size() ? a.spec.dbfs.size() : a.smooth.size();
    if (bins > 0 && fs > 0) snprintf(cur, sizeof cur, "%s \xC2\xB7 %s", formatSpan(span).c_str(), formatResolution(fs / (double)bins).c_str());
    else snprintf(cur, sizeof cur, "%s", formatSpan(span).c_str());
    const float wantW = ImGui::CalcTextSize("10.0 MHz \xC2\xB7 1.22 kHz/bin").x + fp;
    const float readW = std::max(60 * u, std::min(wantW, right - x - 2 * (btnW + gap)));

    const bool atMin = span <= kMinSpanHz * 1.01;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginDisabled(atMin);
    if (ImGui::Button("\xE2\x88\x92##spannarrow", ImVec2(btnW, h))) spanStep(a, false);   // U+2212 minus
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", atMin ? tr("Already at the narrowest view (about 1 kHz)") : tr("Narrower: half the width, with finer detail, about the tuned frequency (key [)"));
    x += btnW + gap;

    if (!S.editing) snprintf(S.buf, sizeof S.buf, "%s", cur);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::SetNextItemWidth(readW);
    const bool enter = ImGui::InputText("##spanval", S.buf, sizeof S.buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    S.editing = ImGui::IsItemActive();
    if (enter) {
        double hz = 0;
        if (std::strcmp(S.buf, cur) == 0) {}   // Enter on the readout itself: nothing to do
        else if (parseSpanHz(S.buf, hz)) spanSetSpan(a, hz, zoomCentreMhz(a, hz));
        else say(a, tr("Type a width such as 50k, 1.2M or 200 kHz (a bare number is kHz; write 1200k rather than 1,200)."), false);
        S.editing = false;
    }
    if (ImGui::IsItemHovered() && !S.editing) {
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(400 * u);
        ImGui::TextUnformatted(tr("Span: how much of the band the spectrum and waterfall show, and how fine one bin is."));
        ImGui::TextUnformatted(tr("Click and type a width such as 50k, 1.2M or 200 kHz (a bare number is kHz), then press Enter."));
        if (w.can) { char b[200]; snprintf(b, sizeof b, tr("Wider than the radio now captures restarts it at a higher rate (up to %s)."), formatSpan(widest).c_str()); ImGui::TextUnformatted(b); }
        else ImGui::TextUnformatted(w.why.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Shortcuts: [ narrower, ] wider. Ctrl+drag across the spectrum or waterfall zooms to that range; Ctrl+double-click fits a signal; right-click for more. The wheel zooms, Ctrl+wheel only the frequency."));
        ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
    x += readW + gap;

    const bool atMax = span >= widest * 0.999;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginDisabled(atMax && !w.can);
    if (ImGui::Button("+##spanwider", ImVec2(btnW, h))) spanStep(a, true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!atMax && span * 2 > fs * 1.0005 && w.can) ImGui::SetTooltip("%s", tr("Wider: twice the width. This is more than the radio captures now, so it restarts at a higher rate (key ])."));
        else if (atMax) ImGui::SetTooltip("%s", w.can ? tr("This is the widest this radio captures. For more, use the sweep.") : w.why.c_str());
        else ImGui::SetTooltip("%s", tr("Wider: twice the width (key ])"));
    }
    x += btnW + gap * 2;

    // "Wider: sweep…" while the last request went past the radio and the view is still at its widest (or the radio is restarting to it)
    const double now = ImGui::GetTime();
    if (S.sweepHint && (now - S.sweepT > 30 || (!S.pending.on && now - S.sweepT > 1 && span < S.sweepAtSpan * 0.99))) S.sweepHint = false;
    const bool sweepOn = S.sweepHint && w.can;
    float sweepW = 0;
    const char* sweepLb = tr("Wider: sweep\xE2\x80\xA6");
    if (sweepOn) sweepW = ImGui::CalcTextSize(sweepLb).x + fp + gap;
    // a fresh restart note goes before the chips (they give way to it)
    const bool noteFresh = !S.note.empty() && now - S.noteT < 8;
    const float noteW = noteFresh && S.noteStrong ? std::min(ImGui::CalcTextSize(S.note.c_str()).x + 2 * gap, (right - x) * 0.6f) : 0;
    const float chipEnd = right - sweepW - noteW;

    // Max, Fit and the zoom tool first (the most useful), then the widths while they fit
    struct Chip { const char* id; double hz; };
    const Chip chips[] = {{"max", 0}, {"fit", -1}, {"tool", -2}, {"10k", 10e3}, {"50k", 50e3}, {"200k", 200e3}, {"1M", 1e6}, {"2.4M", 2.4e6}};
    for (const Chip& c : chips) {
        if (c.hz == -2) {   // the "Zoom to selection" toggle
            if (x + h > chipEnd) break;
            if (zoomToolButton(ImVec2(x, y), h, gZoomTool)) gZoomTool = !gZoomTool;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", gZoomTool ? tr("Zoom to selection is on: drag across a signal on the spectrum or the waterfall to zoom to it (once; Esc cancels). Shortcut: Ctrl+drag.")
                                                                         : tr("Zoom to selection: then drag across a signal on the spectrum or the waterfall to zoom to it. Shortcut: Ctrl+drag."));
            x += h + gap;
            continue;
        }
        const char* lb = c.hz == 0 ? tr("Max") : c.hz < 0 ? tr("Fit") : c.id;
        const double hz = c.hz == 0 ? widest : c.hz;
        if (c.hz > 0 && hz > widest * 1.0005) continue;   // impossible for this radio
        const float cw = ImGui::CalcTextSize(lb).x + fp;
        if (x + cw > chipEnd) break;   // no room: the rest are left out (the readout and the keys still do it)
        const bool on = c.hz >= 0 && std::fabs(span - hz) <= hz * 0.02;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.55f));
        char id[48]; snprintf(id, sizeof id, "%s##chip%s", lb, c.id);
        bool click;
        if (c.hz < 0) {
            ImGui::BeginDisabled(a.smooth.empty());
            click = ImGui::Button(id, ImVec2(cw, h));
            ImGui::EndDisabled();
        } else click = ImGui::Button(id, ImVec2(cw, h));
        if (click) {
            if (c.hz < 0) spanFitSignal(a, listenMhz(a), 0.5e-3);   // at or near the tuned frequency (about 3 kHz either side)
            else spanSetSpan(a, hz, c.hz == 0 ? radioCenterMhz(a) : zoomCentreMhz(a, hz));
        }
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            char b[260];
            if (c.hz < 0) snprintf(b, sizeof b, "%s", tr("Fit: zoom to the signal at the tuned frequency, with room for its sidebands. To fit another signal: Ctrl+double-click it, or right-click it."));
            else if (c.hz == 0) snprintf(b, sizeof b, w.can ? tr("Max: the widest this radio captures, %s (restarts the radio if it runs slower)") : tr("Max: all of the band this source gives, %s"), formatSpan(hz).c_str());
            else if (hz > fs * 1.0005) snprintf(b, sizeof b, tr("Show %s. That is more than the radio captures now, so it restarts at a higher rate."), formatSpan(hz).c_str());
            else snprintf(b, sizeof b, tr("Show %s about the tuned frequency (or the middle of the view when it is not in view)"), formatSpan(hz).c_str());
            ImGui::SetTooltip("%s", b);
        }
        x += cw + gap;
    }
    const float noteEnd = right - sweepW;
    if (sweepOn) {
        ImGui::SetCursorScreenPos(ImVec2(right - sweepW + gap, y));
        if (ImGui::Button(sweepLb)) sweepPresetRange(a, std::max(0.001, S.sweepLo), S.sweepHi);
        if (ImGui::IsItemHovered()) {
            char b[220]; snprintf(b, sizeof b, tr("Open the sweep scanner for %.3f to %.3f MHz: it steps the radio across a range wider than it captures at once"), std::max(0.001, S.sweepLo), S.sweepHi);
            ImGui::SetTooltip("%s", b);
        }
    }
    // the note, for a few seconds, clipped to what is left of the row (a restart in the text colour)
    if (noteFresh && x + 20 * u < noteEnd) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float ty = pos.y + (spanBarHeight() - ImGui::GetTextLineHeight()) * 0.5f;
        dl->PushClipRect(ImVec2(x + gap, pos.y), ImVec2(noteEnd, pos.y + spanBarHeight()), true);
        dl->AddText(ImVec2(x + gap, ty), ImGui::GetColorU32(S.noteStrong ? ImGuiCol_Text : ImGuiCol_TextDisabled), S.note.c_str());
        dl->PopClipRect();
        if (ImGui::IsMouseHoveringRect(ImVec2(x + gap, pos.y), ImVec2(noteEnd, pos.y + spanBarHeight()))) ImGui::SetTooltip("%s", S.note.c_str());
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(keep);
}
