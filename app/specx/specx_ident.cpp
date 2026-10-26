// "What is this?": a click or a drag over a signal on the spectrum or the waterfall (the Identify tool, key I, or Shift+drag) hands a region to the
// engine's capture (dect2/engine/sig_ident.h); a card next to the selection shows what was measured, the modes it could be with how sure each one is
// (the numbers are calibrated, docs/signal-id.md), the evidence on request, and one click switches to the best mode, tuned. "Identify all signals"
// does the same for every signal in the view and lists them in a table.
#include "app.h"
#include "memory/memory_options.h"
#include "dect2/channels/memory_bank.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/specx/specx_measure.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
namespace si = dect2::sigid;
struct IdState {
    bool open = false, all = false;
    double centreRadioHz = 0;   // the samples' centre (radio Hz) when it started
    double offHz = 0, widthHz = 0, rfHz = 0;
    uint64_t seq = 0, seqAll = 0;
    bool have = false, haveAll = false, why = false;
    si::Result res;
    std::vector<si::Capture::Found> found;
    ImVec2 anchor{-1, -1};      // where the selection was made (screen): the card opens beside it
    bool placed = false;
    std::string note;           // the last thing a button did ("Added to Memory.")
    double loMhz = 0, hiMhz = 0;   // the selection, for the band drawn on the plots
    double pendingAt = 0, pendHz = 0, pendBw = 0;   // "Identify selected" of the sweep: the radio was tuned, the capture starts when it has settled
};
IdState I;

std::string fq(double hz) { return dect2::specx::freqText(hz); }

std::string hzWidth(double hz) {
    char b[48];
    if (hz >= 1e6) snprintf(b, sizeof b, "%.3g MHz", hz / 1e6);
    else if (hz >= 1e3) snprintf(b, sizeof b, "%.3g kHz", hz / 1e3);
    else snprintf(b, sizeof b, "%.0f Hz", hz);
    return b;
}

// the measurement in one line: modulation, rate, width
std::string measuredLine(const si::Features& f) {
    std::string s = si::modName(f.mod);
    char b[96];
    if (f.mod == si::ModOfdm && f.ofdmTuSec > 0) { snprintf(b, sizeof b, tr(", useful symbol %.4g us"), f.ofdmTuSec * 1e6); s += b; }
    else if (f.mod == si::ModChirp) { snprintf(b, sizeof b, ", SF%d", f.loraSf); s += b; }
    else if (f.symRate > 0) { snprintf(b, sizeof b, tr(", %.0f baud"), f.symRate); s += b; }
    snprintf(b, sizeof b, tr(", %s wide"), hzWidth(f.obwHz).c_str());
    s += b;
    if (f.duty < 0.97) { snprintf(b, sizeof b, tr(", bursts (%.0f %% on)"), f.duty * 100); s += b; }
    return s;
}

// the verdict in a sentence
std::string verdictLine(const si::Result& r) {
    char b[240];
    if (r.verdict == si::VerdictUnknown || r.cands.empty()) return tr("I do not know what this is: nothing I know fits well enough to say.");
    const si::Candidate& c = r.cands[0];
    if (r.verdict == si::VerdictLikely) snprintf(b, sizeof b, tr("Most likely %s, %.0f %% sure."), c.name.c_str(), c.confidence * 100);
    else if (r.cands.size() > 1) snprintf(b, sizeof b, tr("Could be %s (%.0f %%) or %s (%.0f %%)."), c.name.c_str(), c.confidence * 100, r.cands[1].name.c_str(), r.cands[1].confidence * 100);
    else snprintf(b, sizeof b, tr("Perhaps %s (%.0f %%)."), c.name.c_str(), c.confidence * 100);
    return b;
}

bool canOpen(const si::Candidate& c, const si::Features& f) { const dect2::Memory m = si::toMemory(c, f); return memoryModeIndex(m.mode) >= 0; }

void openMode(App& a, const si::Candidate& c, const si::Features& f) {
    dect2::Memory m = si::toMemory(c, f);
    m.bandwidthHz = 0; m.tags.clear(); m.notes.clear();
    std::string note;
    if (memoryRecallApp(a, m, true, note)) {
        char b[160];
        snprintf(b, sizeof b, tr("Opened %s"), c.name.c_str());
        I.note = std::string(b) + (note.empty() ? "" : "  (" + note + ")");
        I.open = false;
    }
    else I.note = note.empty() ? std::string(tr("Could not open this mode.")) : note;
}

void addMemory(const si::Candidate& c, const si::Features& f) {
    dect2::MemoryBank& b = memoryBank();
    const dect2::MemoryAddResult r = b.add(si::toMemory(c, f), dect2::DupPolicy::Skip);
    if (r.what == dect2::MemoryAdd::Added) { memoryChanged(); I.note = tr("Added to Memory."); }
    else if (r.what == dect2::MemoryAdd::Duplicate) I.note = tr("Already in Memory.");
    else I.note = tr("The memory bank is full.");
}

void row(const char* k, const std::string& v) {
    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", tr(k));
    ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
}

// a confidence bar with the percentage inside
void confBar(double conf, float w) {
    char b[24];
    snprintf(b, sizeof b, "%.0f %%", conf * 100);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, conf >= 0.6 ? ImVec4(0.30f, 0.65f, 0.40f, 1) : conf >= 0.3 ? ImVec4(0.78f, 0.62f, 0.25f, 1) : ImVec4(0.55f, 0.55f, 0.58f, 1));
    ImGui::ProgressBar((float)conf, ImVec2(w, ImGui::GetTextLineHeight() + 2 * gUi), b);
    ImGui::PopStyleColor();
}

void startCapture(App& a) {
    if (!a.engine.running() || a.engine.sampleRate() <= 0) return;
    const double rate = a.engine.sampleRate();
    if (I.all) a.engine.sigid().startAll(rate, I.rfHz, si::captureSeconds(rate));
    else a.engine.sigid().start(rate, I.offHz, I.widthHz, I.rfHz, si::captureSeconds(rate));
}

void begin(App& a, bool all) {
    I.open = true; I.all = all; I.have = I.haveAll = false; I.why = false; I.note.clear(); I.placed = false;
    I.anchor = ImGui::GetMousePos();
    startCapture(a);
}
} // namespace

bool identifyRegionMhz(double& loMhz, double& hiMhz) {
    if (!I.open || I.all || I.widthHz <= 0) return false;
    loMhz = I.loMhz; hiMhz = I.hiMhz;
    return true;
}

void identifyStart(App& a, double sampleCentreRadioHz, double offHz, double widthHz, double rfOfCentreHz) {
    I = IdState();
    I.centreRadioHz = sampleCentreRadioHz; I.offHz = offHz; I.widthHz = widthHz; I.rfHz = rfOfCentreHz;
    I.loMhz = (sampleCentreRadioHz + offHz - widthHz / 2) / 1e6; I.hiMhz = (sampleCentreRadioHz + offHz + widthHz / 2) / 1e6;
    begin(a, false);
}

void identifyRegion(App& a, double loMhz, double hiMhz) {
    const double rc = radioCenterMhz(a) * 1e6;
    if (hiMhz < loMhz) std::swap(loMhz, hiMhz);
    identifyStart(a, rc, (loMhz + hiMhz) / 2 * 1e6 - rc, (hiMhz - loMhz) * 1e6, rc);
}

void identifyAt(App& a, double clickMhz) {
    const double rc = radioCenterMhz(a) * 1e6, click = clickMhz * 1e6;
    double lo = click - 12.5e3, hi = click + 12.5e3;   // nothing found under the click: a 25 kHz channel around it
    const std::vector<double> x = xs(a);
    if (x.size() >= 16 && a.smooth.size() >= x.size()) {
        dect2::specx::SpecView v;
        v.db = a.smooth.data(); v.n = (int)x.size(); v.f0Hz = x[0] * 1e6; v.binHz = (x[1] - x[0]) * 1e6; v.enbw = 1;
        for (const auto& m : dect2::specx::detect(v, 6, 3))
            if (click >= m.loHz && click <= m.hiHz) { lo = m.loHz; hi = m.hiHz; break; }
    }
    identifyStart(a, rc, (lo + hi) / 2 - rc, hi - lo, rc);
}

void identifyTuned(App& a, double freqHz, double bwHz) {
    I = IdState();
    a.freqMhz = freqHz / 1e6;
    if (a.engine.running()) { a.tune.centerHz = freqHz; a.engine.retune(a.tune); } else startReceiver(a);
    I.open = true; I.placed = false;
    I.anchor = ImGui::GetMousePos();
    I.pendingAt = ImGui::GetTime() + 0.8; I.pendHz = freqHz; I.pendBw = bwHz;
    I.rfHz = freqHz; I.centreRadioHz = freqHz; I.widthHz = std::max(bwHz * 1.2, 12.5e3);
}

void identifyAll(App& a) {
    const ImVec2 keep = I.anchor;
    const bool wasOpen = I.open;
    I = IdState();
    const double rc = radioCenterMhz(a) * 1e6;
    I.centreRadioHz = rc; I.rfHz = rc;
    begin(a, true);
    if (wasOpen) I.anchor = keep;   // from the button of an open card: the window stays where it is
}

void identifyWindow(App& a) {
    if (!I.open) return;
    si::Capture& cap = a.engine.sigid();
    if (I.pendingAt > 0 && ImGui::GetTime() >= I.pendingAt && a.engine.running() && a.engine.sampleRate() > 0) {   // the radio has settled on the signal: look at it
        const double hz = I.pendHz, bw = I.pendBw, rc = radioCenterMhz(a) * 1e6;
        const ImVec2 anchor = I.anchor;
        identifyStart(a, rc, hz - rc, std::max(bw * 1.2, 12.5e3), rc);
        I.anchor = anchor; I.placed = false;
    }
    if (I.all) { std::vector<si::Capture::Found> f; if (cap.resultAll(f, I.seqAll)) { I.found = std::move(f); I.haveAll = true; } }
    else { si::Result r; if (cap.result(r, I.seq)) { I.res = std::move(r); I.have = true; } }
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (!I.placed) {   // beside the selection, inside the window
        const float x = std::min(std::max(0.f, I.anchor.x + 14 * gUi), std::max(0.f, disp.x - 460 * gUi));
        const float y = std::min(std::max(0.f, I.anchor.y + 14 * gUi), std::max(0.f, disp.y - 360 * gUi));
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        I.placed = true;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(420 * gUi, 0), ImVec2(640 * gUi, disp.y * 0.9f));
    const bool shown = ImGui::Begin(trw(I.all ? "Signals in the view" : "Identify signal"), &I.open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    if (shown) {
        const int st = cap.state();
        if (I.pendingAt > 0 && ImGui::GetTime() < I.pendingAt) ImGui::TextUnformatted(tr("Working it out..."));
        else if (!a.engine.running() && st != 3) ImGui::TextUnformatted(tr("Start the receiver to identify a signal."));
        else if (st == 1) ImGui::ProgressBar((float)cap.progress(), ImVec2(-1, 0), tr("collecting samples"));
        else if (st == 2) ImGui::TextUnformatted(tr("Working it out..."));
        if (I.all) {
            if (I.haveAll) {
                if (I.found.empty()) ImGui::TextUnformatted(tr("No signal stands out of the noise here."));
                else if (ImGui::BeginTable("##sidall", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                    ImGui::TableSetupColumn(tr("Frequency")); ImGui::TableSetupColumn(tr("Width")); ImGui::TableSetupColumn(tr("Best guess")); ImGui::TableSetupColumn(tr("Sure")); ImGui::TableSetupColumn("");
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < I.found.size(); i++) {
                        const si::Result& r = I.found[i].res;
                        ImGui::PushID((int)i);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(fq(r.f.valid ? r.f.rfHz : I.rfHz + I.found[i].region.centreHz).c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(hzWidth(r.f.valid ? r.f.obwHz : I.found[i].region.bwHz).c_str());
                        ImGui::TableNextColumn();
                        if (r.f.valid && !r.cands.empty() && r.verdict != si::VerdictUnknown) ImGui::TextUnformatted(r.cands[0].name.c_str());
                        else ImGui::TextDisabled("%s", r.f.valid ? tr("unknown") : "-");
                        if (ImGui::IsItemHovered() && r.f.valid) ImGui::SetTooltip("%s", measuredLine(r.f).c_str());
                        ImGui::TableNextColumn();
                        if (r.f.valid && !r.cands.empty()) confBar(r.cands[0].confidence, 70 * gUi);
                        ImGui::TableNextColumn();
                        if (r.f.valid && !r.cands.empty() && r.verdict != si::VerdictUnknown && canOpen(r.cands[0], r.f)) {
                            if (ImGui::SmallButton(tr("Open"))) openMode(a, r.cands[0], r.f);
                            ImGui::SameLine();
                            if (ImGui::SmallButton(tr("Add"))) addMemory(r.cands[0], r.f);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
        } else {
            if (I.widthHz > 0) ImGui::TextDisabled(tr("Region %s, %s wide"), fq(I.rfHz + I.offHz).c_str(), fq(I.widthHz).c_str());
            else ImGui::TextDisabled(tr("The strongest signal around %s"), fq(I.rfHz).c_str());
            if (I.have) {
                const si::Features& f = I.res.f;
                if (!f.valid) ImGui::TextWrapped("%s", f.why.empty() ? tr("Nothing to identify here.") : f.why.c_str());
                else {
                    ImGui::Separator();
                    ImGui::TextWrapped("%s", measuredLine(f).c_str());
                    ImGui::PushStyleColor(ImGuiCol_Text, I.res.verdict == si::VerdictLikely ? ImVec4(0.55f, 0.88f, 0.60f, 1) : I.res.verdict == si::VerdictAmbiguous ? ImVec4(0.95f, 0.80f, 0.45f, 1) : ImVec4(0.75f, 0.75f, 0.78f, 1));
                    ImGui::TextWrapped("%s", verdictLine(I.res).c_str());
                    ImGui::PopStyleColor();
                    if (!I.res.cands.empty() && I.res.verdict != si::VerdictUnknown) {
                        const si::Candidate& top = I.res.cands[0];
                        if (canOpen(top, f)) {
                            char b[160];
                            snprintf(b, sizeof b, tr("Open in %s"), top.name.c_str());
                            if (ImGui::Button(b)) openMode(a, top, f);
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Switch to %s and tune to %s"), top.name.c_str(), fq(f.rfHz).c_str());
                            ImGui::SameLine();
                        }
                        if (ImGui::Button(tr("Add to memory"))) addMemory(top, f);
                    }
                    if (!I.res.cands.empty()) {
                        ImGui::Spacing();
                        ImGui::TextDisabled("%s", tr("Candidates"));
                        const float nameW = 190 * gUi;
                        for (size_t i = 0; i < I.res.cands.size(); i++) {
                            const si::Candidate& c = I.res.cands[i];
                            ImGui::PushID((int)i);
                            ImGui::SetNextItemWidth(nameW);
                            ImGui::TextUnformatted(c.name.c_str());
                            ImGui::SameLine(nameW + 8 * gUi);
                            confBar(c.confidence, 140 * gUi);
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.why.c_str());
                            if (c.syncTried) { ImGui::SameLine(); ImGui::TextDisabled(tr(c.syncHits ? "(sync %dx)" : "(no sync)"), c.syncHits); }
                            if (i > 0 && canOpen(c, f)) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Open"))) openMode(a, c, f); }
                            ImGui::PopID();
                        }
                    }
                    if (ImGui::TreeNode(tr("Measured"))) {
                        if (ImGui::BeginTable("##sidf", 2, ImGuiTableFlags_SizingFixedFit)) {
                            char b[128];
                            row(N_("Centre"), fq(f.rfHz));
                            snprintf(b, sizeof b, "%s / %s / %s", fq(f.bw3Hz).c_str(), fq(f.bw20Hz).c_str(), fq(f.bw26Hz).c_str()); row(N_("Width -3 / -20 / -26 dB"), b);
                            row(N_("Occupied (99 %)"), fq(f.obwHz));
                            snprintf(b, sizeof b, "%.1f dB", f.snrDb); row("SNR", b);
                            if (f.duty < 0.97) {
                                snprintf(b, sizeof b, tr("bursts, %.0f %% on, %d of about %.3g ms"), f.duty * 100, f.bursts, f.burstMs);
                                std::string s = b;
                                if (f.burstPeriodMs > 0) { snprintf(b, sizeof b, tr(", every %.3g ms"), f.burstPeriodMs); s += b; }
                                row(N_("On the air"), s);
                            } else row(N_("On the air"), tr("continuous"));
                            row(N_("Modulation"), si::modName(f.mod));
                            snprintf(b, sizeof b, tr("%.2f (1 constant, 2 noise-like)"), f.envKurt); row(N_("Envelope"), b);
                            if (f.symRate > 0) {
                                static const char* const kSrc[] = {"", "|x|^2 line", "FM discriminator line", "x^n lines", "chirp length", "OFDM symbol"};
                                snprintf(b, sizeof b, "%.1f Bd (%s, %.0f dB)", f.symRate, kSrc[f.symSrc < 6 ? f.symSrc : 0], f.symLineDb); row(N_("Symbol rate"), b);
                            }
                            if (f.fskLevels == 4) { snprintf(b, sizeof b, "%+.0f / %+.0f / %+.0f / %+.0f Hz", f.devHz[0], f.devHz[1], f.devHz[2], f.devHz[3]); row("FSK levels", b); }
                            if (f.fskLevels == 2) { snprintf(b, sizeof b, "%+.0f / %+.0f Hz", f.devHz[0], f.devHz[1]); row("FSK levels", b); }
                            if (f.mod == si::ModOfdm) { snprintf(b, sizeof b, "Tu %.4g us, guard %.4g us", f.ofdmTuSec * 1e6, f.ofdmTgSec * 1e6); row("OFDM", b); }
                            if (f.mod == si::ModChirp) { snprintf(b, sizeof b, "SF%d, %s", f.loraSf, fq(f.chirpBwHz).c_str()); row("LoRa", b); }
                            ImGui::EndTable();
                        }
                        ImGui::TreePop();
                    }
                    if (ImGui::TreeNode(tr("Why?"))) {
                        if (!I.res.priors.empty()) {
                            ImGui::TextDisabled("%s", tr("The frequency list has here:"));
                            for (const std::string& p : I.res.priors) ImGui::BulletText("%s", p.c_str());
                        }
                        if (!I.res.cands.empty()) {
                            ImGui::TextDisabled(tr("Evidence for %s:"), I.res.cands[0].name.c_str());
                            for (const std::string& e : I.res.cands[0].evidence) ImGui::BulletText("%s", e.c_str());
                        }
                        ImGui::TextDisabled("%s", tr("A score is the share of the evidence that holds; the percentages are calibrated on Eye of SaurCom's own test signals, not on every real signal."));
                        ImGui::TreePop();
                    }
                }
            }
        }
        if (!I.note.empty()) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1), "%s", I.note.c_str());
        ImGui::Separator();
        if (!I.all && I.have && I.res.f.valid && ImGui::Button(tr("Copy as text"))) { ImGui::SetClipboardText(si::toText(I.res).c_str()); I.note = tr("Copied"); }
        if (!I.all && I.have && I.res.f.valid) ImGui::SameLine();
        if (ImGui::Button(tr("Again")) && a.engine.running()) { I.have = I.haveAll = false; I.note.clear(); startCapture(a); }
        ImGui::SameLine();
        if (!I.all) { if (ImGui::Button(tr("Identify all signals")) && a.engine.running()) identifyAll(a); ImGui::SameLine(); }
        if (ImGui::Button(tr("Close"))) I.open = false;
    }
    ImGui::End();
    if (!I.open && a.engine.sigid().state() != 0) a.engine.sigid().cancel();
}
