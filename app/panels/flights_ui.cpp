// The flight history window (View > Flight history): what has been over the receiver, what each aircraft does, and what stands out.
//
// Four screens. Overview answers "what is in my airspace, and has anything changed"; Flights is the log with the track of whichever one
// is picked; Aircraft is the dossier, which is the one place the user writes rather than reads; Patterns is the aggregate view.
//
// The recorder is driven from here, once a frame, whichever screen the user is on, so flights go on being recorded while they look at
// something else. The store is opened the first time this runs.
#include "../app.h"
#include "../state/receivers.h"
#include "photo_links_ui.h"
#include "dect2/airports/airports.h"
#include "dect2/common/u8file.h"
#include "dect2/flights/flights_db.h"
#include "dect2/flights/flights_rec.h"
#include "../gfx/gfx.h"
#include "../platform/platform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// the identity helpers of the ADS-B screens (adsb_ui.cpp); its own header is private to those files
namespace adsbui {
std::string registrationOf(uint32_t icao);
std::string operatorOf(uint32_t icao);
std::string countryCode(uint32_t icao);
}

namespace {
namespace fl = dect2::flights;
namespace fs = std::filesystem;

// ImGui's own InputText over a std::string, as the other panels do it
int resizeCb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize((size_t)d->BufTextLen);
        d->Buf = s->data();
    }
    return 0;
}
bool txt(const char* id, const char* hint, std::string& s) {
    return ImGui::InputTextWithHint(id, hint, s.data(), s.capacity() + 1, ImGuiInputTextFlags_CallbackResize, resizeCb, &s);
}
bool txtMulti(const char* id, std::string& s, ImVec2 size) {
    return ImGui::InputTextMultiline(id, s.data(), s.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize, resizeCb, &s);
}

struct State {
    bool started = false;
    std::string err;
    int tab = 0;
    int tabWant = -1;              // a screen asked to jump to another tab; -1 the rest of the time, so the tab bar stays the user's
    // what the screens are filtered to
    int range = 2;                 // 0 today, 1 this week, 2 this month, 3 everything
    int kind = -1;
    char airport[12] = "";
    char callsign[12] = "";
    bool emergenciesOnly = false;
    // what is selected
    int64_t flightSel = 0;
    uint32_t acSel = 0;
    // the dossier being edited, held while the user types
    uint32_t editFor = 0;
    std::string notes, tags;
    char factKey[48] = "", factVal[128] = "", note[160] = "";
    std::string msg;
    double msgT = 0;
};
State S;

void say(const std::string& s) { S.msg = s; S.msgT = ImGui::GetTime(); }

// ---- thumbnails of the pictures the user attached
//
// Decoded from their own files on demand, one new picture per frame, so attaching a folder's worth does not stall a frame. A picture the
// user has since moved or deleted simply shows its path.
struct Thumb {
    gfx::Image* tex = nullptr;
    int w = 0, h = 0;
    bool tried = false;
};
std::map<std::string, Thumb> gThumbs;
bool gThumbThisFrame = false;

const Thumb* thumbFor(const std::string& path) {
    auto it = gThumbs.find(path);
    if (it != gThumbs.end()) return &it->second;
    if (gThumbThisFrame || !gGfx) return nullptr;   // next frame
    gThumbThisFrame = true;
    Thumb t;
    t.tried = true;
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    if (plat::decodeImage(path, w, h, px) && w > 0 && h > 0 && (size_t)w * (size_t)h <= px.size()) {
        const int kMax = 160;
        const int step = std::max(1, (std::max(w, h) + kMax - 1) / kMax);
        t.w = std::max(1, w / step);
        t.h = std::max(1, h / step);
        std::vector<uint32_t> small((size_t)t.w * (size_t)t.h, 0xFF000000u);
        for (int y = 0; y < t.h; y++)
            for (int x = 0; x < t.w; x++) small[(size_t)y * t.w + x] = px[(size_t)(y * step) * w + (size_t)(x * step)];
        t.tex = gGfx->createImage(t.w, t.h, 0xFF000000u);
        if (t.tex) t.tex->update(0, 0, t.w, t.h, small.data());
    }
    return &(gThumbs[path] = t);
}

void forgetThumb(const std::string& path) {
    auto it = gThumbs.find(path);
    if (it == gThumbs.end()) return;
    delete it->second.tex;
    gThumbs.erase(it);
}

std::string dir() { return plat::dataDir() + "/flights"; }

void start() {
    S.started = true;
    std::error_code ec;
    fs::create_directories(fs::path(reinterpret_cast<const char8_t*>(dir().c_str())), ec);
    if (!fl::global().open(dir() + "/flights.db", &S.err)) return;
    fl::global().prune();
    fl::Recorder& r = fl::globalRecorder();
    r.setStore(&fl::global());
    r.setAirports(&dect2::AirportDb::builtin());
    // the identity of an aircraft comes from the program's own registration database
    r.setFactsSource([](uint32_t icao, fl::AircraftFacts& out) {
        out.icao = icao;
        out.registration = adsbui::registrationOf(icao);
        out.operatorName = adsbui::operatorOf(icao);
    });
}

// ---- small drawing helpers

std::string hhmm(double t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    localtime_s(&g, &tt);
#else
    localtime_r(&tt, &g);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &g);
    return b;
}

std::string hours(double sec) {
    char b[32];
    if (sec < 3600) snprintf(b, sizeof b, "%.0f min", sec / 60);
    else snprintf(b, sizeof b, "%.1f h", sec / 3600);
    return b;
}

std::string icaoHex(uint32_t i) {
    char b[8];
    snprintf(b, sizeof b, "%06X", i & 0xFFFFFF);
    return b;
}

// a name for an aircraft: its registration if known, else the hex address
std::string nameOf(uint32_t icao, const std::string& reg) { return reg.empty() ? icaoHex(icao) : reg; }

// a row of bars, for the hour and weekday questions
void barRow(const char* label, const std::vector<fl::Bucket>& v, int bins, const char* const* names) {
    int64_t mx = 1;
    for (const auto& b : v) mx = std::max(mx, b.count);
    std::vector<int64_t> byBin((size_t)bins, 0);
    for (const auto& b : v) if (b.bin >= 0 && b.bin < bins) byBin[(size_t)b.bin] = b.count;
    ImGui::TextDisabled("%s", label);
    const float w = std::max(14.f, (ImGui::GetContentRegionAvail().x - 40 * gUi) / (float)bins - 2 * gUi);
    const float h = 54 * gUi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    for (int i = 0; i < bins; i++) {
        const float frac = (float)byBin[(size_t)i] / (float)mx;
        const float x = o.x + i * (w + 2 * gUi);
        dl->AddRectFilled(ImVec2(x, o.y + h - 12 * gUi), ImVec2(x + w, o.y + h), IM_COL32(255, 255, 255, 14));
        if (frac > 0)
            dl->AddRectFilled(ImVec2(x, o.y + (h - 12 * gUi) * (1 - frac)), ImVec2(x + w, o.y + h - 12 * gUi), ImGui::GetColorU32(pal::accent()));
        if (names && w > 16 * gUi) dl->AddText(ImVec2(x + 1, o.y + h - 11 * gUi), IM_COL32(170, 175, 180, 255), names[i]);
        else if (!names && (i % 6) == 0) {
            char lb[8];
            snprintf(lb, sizeof lb, "%d", i);
            dl->AddText(ImVec2(x + 1, o.y + h - 11 * gUi), IM_COL32(170, 175, 180, 255), lb);
        }
    }
    ImGui::Dummy(ImVec2(0, h + 4 * gUi));
    // the numbers, for the hour or day that stands out
    int top = 0;
    for (int i = 1; i < bins; i++) if (byBin[(size_t)i] > byBin[(size_t)top]) top = i;
    if (byBin[(size_t)top] > 0) {
        if (names) ImGui::TextDisabled("busiest: %s (%lld)", names[top], (long long)byBin[(size_t)top]);
        else ImGui::TextDisabled("busiest hour: %02d:00 (%lld)", top, (long long)byBin[(size_t)top]);
    }
}

const char* const kDayNames[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};

fl::Filter currentFilter() {
    fl::Filter f;
    const double now = (double)std::time(nullptr);
    if (S.range == 0) f.since = now - 86400;
    else if (S.range == 1) f.since = now - 7 * 86400;
    else if (S.range == 2) f.since = now - 30 * 86400;
    f.kind = S.kind;
    f.airport = S.airport;
    f.callsign = S.callsign;
    f.emergenciesOnly = S.emergenciesOnly;
    f.limit = 500;
    return f;
}

void filterRow() {
    ImGui::SetNextItemWidth(110 * gUi);
    const char* ranges[] = {"Last day", "Last week", "Last month", "Everything"};
    ImGui::Combo("##frange", &S.range, ranges, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * gUi);
    const char* kinds[] = {"Any kind", "unknown", "airline", "cargo", "private", "medical", "military", "survey", "skydive", "training"};
    int k = S.kind + 1;
    if (ImGui::Combo("##fkind", &k, kinds, 10)) S.kind = k - 1;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    ImGui::InputTextWithHint("##fap", "airport", S.airport, sizeof S.airport);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputTextWithHint("##fcs", "call sign", S.callsign, sizeof S.callsign);
    ImGui::SameLine();
    ImGui::Checkbox("Emergencies", &S.emergenciesOnly);
}

void rankTable(const char* id, const char* what, const std::vector<fl::Ranked>& v, bool showHours) {
    if (v.empty()) { ImGui::TextDisabled("nothing yet"); return; }
    if (!ImGui::BeginTable(id, showHours ? 3 : 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn(what);
    ImGui::TableSetupColumn("Flights");
    if (showHours) ImGui::TableSetupColumn("Airborne");
    ImGui::TableHeadersRow();
    for (const auto& r : v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.label.empty() ? r.key.c_str() : r.label.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%lld", (long long)r.count);
        if (showHours) { ImGui::TableNextColumn(); ImGui::TextUnformatted(hours(r.seconds).c_str()); }
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- overview

void overview(App& a) {
    fl::Db& db = fl::global();
    const fl::Stats st = db.stats();
    const fl::Recorder::Status rs = fl::globalRecorder().status();
    bool on = rs.enabled;
    if (ImGui::Checkbox("Record flights", &on)) fl::globalRecorder().setEnabled(on);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu being recorded now, %lld written this session", rs.openFlights, (long long)rs.flightsWritten);
    if (!rs.lastFlight.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(last: %s)", rs.lastFlight.c_str()); }
    if (!S.err.empty()) ImGui::TextColored(pal::warnAmber(), "the store could not be opened: %s", S.err.c_str());
    ImGui::Text("%lld flights of %lld aircraft, %lld track points, %.1f MB", (long long)st.flights, (long long)st.aircraft,
                (long long)st.points, st.bytes / 1048576.0);
    if (st.oldest > 0) { ImGui::SameLine(); ImGui::TextDisabled("since %s", hhmm(st.oldest).c_str()); }
    ImGui::Separator();
    filterRow();
    const fl::Filter f = currentFilter();

    if (ImGui::BeginTable("##ovcols", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("In your airspace the most");
        const auto top = db.topAircraft(f, 12);
        if (ImGui::BeginTable("##ovtop", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Aircraft");
            ImGui::TableSetupColumn("Flights");
            ImGui::TableSetupColumn("Airborne");
            ImGui::TableSetupColumn("Mean alt");
            ImGui::TableHeadersRow();
            for (const auto& r : top) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(r.label.empty() ? r.key.c_str() : r.label.c_str(), S.acSel == r.icao)) { S.acSel = r.icao; S.tabWant = 2; }
                ImGui::TableNextColumn(); ImGui::Text("%lld", (long long)r.count);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(hours(r.seconds).c_str());
                ImGui::TableNextColumn();
                if (r.value > 0) ImGui::Text("%.0f ft", r.value); else ImGui::TextDisabled("-");
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("What kind of flying");
        rankTable("##ovkind", "Kind", db.byKind(f), true);

        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Busiest airports");
        rankTable("##ovap", "Airport", db.topAirports(f, 10), false);
        ImGui::Spacing();
        ImGui::TextUnformatted("Busiest operators");
        rankTable("##ovop", "Operator", db.topOperators(f, 8), false);
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("What stood out");
    const auto an = db.anomalies(f, 12);
    if (an.empty()) ImGui::TextDisabled("nothing unusual in this period");
    else if (ImGui::BeginTable("##ovan", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("When");
        ImGui::TableSetupColumn("Aircraft");
        ImGui::TableSetupColumn("Call sign");
        ImGui::TableSetupColumn("Why", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& x : an) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(hhmm(x.t).c_str());
            ImGui::TableNextColumn();
            if (ImGui::Selectable(nameOf(x.icao, x.registration).c_str(), false)) { S.acSel = x.icao; S.tabWant = 2; }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(x.callsign.c_str());
            ImGui::TableNextColumn();
            if (x.score >= 1) ImGui::TextColored(pal::warnAmber(), "%s", x.reason.c_str()); else ImGui::TextUnformatted(x.reason.c_str());
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------- the flight log

void flightDetail(App& a, const fl::Flight& g) {
    ImGui::TextUnformatted(("Flight of " + nameOf(g.icao, adsbui::registrationOf(g.icao)) + (g.callsign.empty() ? "" : " as " + g.callsign)).c_str());
    ImGui::TextDisabled("%s to %s", hhmm(g.tStart).c_str(), hhmm(g.tEnd).c_str());
    ImGui::Text("%s airborne, %.0f nm along the track, %zu positions", hours(g.airborneSec).c_str(), g.distanceNm, g.points);
    if (g.hasAlt) ImGui::Text("Altitude: up to %d ft, cruised at %d ft", g.maxAltFt, g.cruiseAltFt);
    if (g.hasSpeed) ImGui::Text("Speed: up to %.0f kt, cruised at %.0f kt", g.maxSpeedKt, g.cruiseSpeedKt);
    if (g.hasRange) ImGui::Text("Range from here: %.0f to %.0f nm", g.minRangeNm, g.maxRangeNm);
    ImGui::Text("From: %s %s %s", fl::endKindName(g.origin.kind), g.origin.icao.c_str(), g.origin.runway.empty() ? "" : ("rwy " + g.origin.runway).c_str());
    ImGui::Text("To:   %s %s %s", fl::endKindName(g.destination.kind), g.destination.icao.c_str(), g.destination.runway.empty() ? "" : ("rwy " + g.destination.runway).c_str());
    if (g.kind) ImGui::Text("Looks like: %s (%.0f%% sure) - %s", fl::operationKindName((fl::OperationKind)g.kind), 100 * g.kindConfidence, g.kindBasis.c_str());
    if (!g.squawk.empty()) {
        ImGui::Text("Squawk %s", g.squawk.c_str());
        if (g.emergencySquawk) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "emergency"); }
    }
    // the phases, as a share of the flight
    uint32_t tot = 0;
    for (uint32_t s : g.phaseSec) tot += s;
    if (tot) {
        ImGui::Spacing();
        ImGui::TextDisabled("Time in each phase");
        for (int i = 1; i < 8; i++) {
            if (!g.phaseSec[i]) continue;
            ImGui::Text("%-9s %5u s", fl::phaseName((fl::Phase)i), g.phaseSec[i]);
            ImGui::SameLine();
            ImGui::ProgressBar((float)g.phaseSec[i] / (float)tot, ImVec2(160 * gUi, 12 * gUi), "");
        }
    }
    if (g.approach.ok) {
        ImGui::Spacing();
        ImGui::TextDisabled("Approach to %s runway %s, against a %.1f degree path", g.approach.airport.c_str(), g.approach.runway.c_str(), g.approach.glideDeg);
        ImGui::Text("Off the centreline: %.2f nm on average, %.2f nm at worst", g.approach.meanLateralNm, g.approach.maxLateralNm);
        ImGui::Text("Off the glide path: %+.0f ft on average, %+.0f ft at worst", g.approach.meanVerticalFt, g.approach.maxVerticalFt);
        if (g.approach.crossed) ImGui::Text("Crossed the threshold at %.0f ft", g.approach.thresholdCrossFt);
        if (g.approach.alignedFromNm > 0) ImGui::Text("Lined up from %.1f nm out", g.approach.alignedFromNm);
    }
    ImGui::Spacing();
    if (ImGui::SmallButton("Copy track (CSV)")) { ImGui::SetClipboardText(fl::global().exportTrack(g.id, "csv").c_str()); say("track copied"); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Save GPX...")) {
        const std::string p = saveFileDialog((icaoHex(g.icao) + "_" + std::to_string((long long)g.tStart) + ".gpx").c_str());
        if (!p.empty()) {
            const std::string gpx = fl::global().exportTrack(g.id, "gpx");
            if (FILE* fp = dect2::u8fopen(p, "wb")) { fwrite(gpx.data(), 1, gpx.size(), fp); fclose(fp); say("saved"); }
            else say("could not write that file");
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open its aircraft")) { S.acSel = g.icao; S.tabWant = 2; }
}

void flightsTab(App& a) {
    filterRow();
    fl::Db& db = fl::global();
    fl::Filter f = currentFilter();
    const int64_t n = db.countFlights(f);
    ImGui::SameLine();
    ImGui::TextDisabled("%lld flights", (long long)n);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy list (CSV)")) { ImGui::SetClipboardText(db.exportFlights(f, "csv").c_str()); say("list copied"); }
    const auto v = db.flights(f);
    const float lw = std::min(560 * gUi, ImGui::GetContentRegionAvail().x * 0.55f);
    if (ImGui::BeginChild("##fllist", ImVec2(lw, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("##fltab", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("When");
            ImGui::TableSetupColumn("Aircraft");
            ImGui::TableSetupColumn("Call sign");
            ImGui::TableSetupColumn("From");
            ImGui::TableSetupColumn("To");
            ImGui::TableSetupColumn("Alt");
            ImGui::TableHeadersRow();
            for (const auto& g : v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char lbl[64];
                snprintf(lbl, sizeof lbl, "%s##f%lld", hhmm(g.tStart).c_str(), (long long)g.id);
                if (ImGui::Selectable(lbl, S.flightSel == g.id, ImGuiSelectableFlags_SpanAllColumns)) S.flightSel = g.id;
                ImGui::TableNextColumn(); ImGui::TextUnformatted(nameOf(g.icao, adsbui::registrationOf(g.icao)).c_str());
                ImGui::TableNextColumn();
                if (g.emergencySquawk || g.emergency > 0) ImGui::TextColored(pal::warnAmber(), "%s", g.callsign.c_str());
                else ImGui::TextUnformatted(g.callsign.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(g.origin.icao.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(g.destination.icao.c_str());
                ImGui::TableNextColumn();
                if (g.hasAlt) ImGui::Text("%d", g.cruiseAltFt ? g.cruiseAltFt : g.maxAltFt); else ImGui::TextDisabled("-");
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##fldet", ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        fl::Flight g;
        if (S.flightSel && db.flight(S.flightSel, g)) flightDetail(a, g);
        else ImGui::TextDisabled("pick a flight on the left");
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the dossier

void dossier(App& a, uint32_t icao) {
    fl::Db& db = fl::global();
    fl::AircraftRecord r;
    if (!db.aircraft(icao, r)) { ImGui::TextDisabled("nothing recorded for this aircraft yet"); return; }
    // the editable parts are loaded once per selection, so typing is not overwritten each frame
    if (S.editFor != icao) {
        S.editFor = icao;
        S.notes = r.notes;
        S.note[0] = 0;
        S.tags.clear();
        for (size_t i = 0; i < r.tags.size(); i++) { if (i) S.tags += ", "; S.tags += r.tags[i]; }
    }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(icaoHex(icao).c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextUnformatted(r.registration.empty() ? "(registration not known)" : r.registration.c_str());
    if (!r.type.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", r.type.c_str()); }
    if (!r.operatorName.empty()) { ImGui::SameLine(); ImGui::TextDisabled("- %s", r.operatorName.c_str()); }
    photolinks::aircraftButton(icao, r.registration, "");

    ImGui::Separator();
    if (ImGui::BeginTable("##dosfacts", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("Seen %lld times, %s airborne", (long long)r.flightCount, hours(r.airborneSec).c_str());
        ImGui::TextDisabled("(time heard overhead, not the airframe's total)");
        ImGui::Text("First heard %s", hhmm(r.firstSeen).c_str());
        ImGui::Text("Last heard  %s", hhmm(r.lastSeen).c_str());
        ImGui::Text("Along the track: %.0f nm", r.distanceNm);
        if (r.meanCruiseAltFt > 0) ImGui::Text("Usually cruises %.0f ft at %.0f kt", r.meanCruiseAltFt, r.meanCruiseSpeedKt);
        if (r.maxAltFt) ImGui::Text("Highest seen %d ft, fastest %.0f kt", r.maxAltFt, r.maxSpeedKt);
        ImGui::TableNextColumn();
        if (!r.homeIcao.empty()) ImGui::Text("Begins or ends at %s (%lld times)", r.homeIcao.c_str(), (long long)r.homeCount);
        if (r.meanGroundSec > 0) ImGui::Text("Average time on the ground: %s", hours(r.meanGroundSec).c_str());
        const fl::Regularity rg = db.regularity(icao);
        if (rg.flights > 1)
            ImGui::Text("Paths flown: %lld, most of them (%.0f%%) the same one", (long long)rg.distinctRoutes, 100 * rg.share);
        ImGui::Text("Rotorcraft: %s", r.rotorcraft ? "yes" : "no");
        // what kind of flying, and the user's say over it
        ImGui::Spacing();
        ImGui::Text("Kind of flying: %s", fl::operationKindName(r.effectiveKind()));
        if (r.kindUser >= 0) { ImGui::SameLine(); ImGui::TextDisabled("(your answer)"); }
        if (!r.kindBasis.empty()) ImGui::TextDisabled("the program's reading: %s - %s", fl::operationKindName(r.kind), r.kindBasis.c_str());
        const char* kinds[] = {"(the program's answer)", "unknown", "airline", "cargo", "private", "medical", "military", "survey", "skydive", "training"};
        int sel = r.kindUser + 1;
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::Combo("Set the kind", &sel, kinds, 10)) db.setKindOverride(icao, sel - 1);
        ImGui::EndTable();
    }

    ImGui::Separator();
    const fl::Filter one = [&] { fl::Filter f; f.icao = icao; return f; }();
    if (ImGui::BeginTable("##doshist", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        barRow("Hour of the day", db.hourHistogram(one), 24, nullptr);
        ImGui::TableNextColumn();
        barRow("Day of the week", db.weekdayHistogram(one), 7, kDayNames);
        ImGui::EndTable();
    }
    const auto routes = db.topRoutes(one, 8);
    if (!routes.empty()) {
        ImGui::TextDisabled("Where it goes");
        rankTable("##dosroutes", "Route", routes, false);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Your own notes on this aircraft");
    ImGui::TextDisabled("Nothing here is sent anywhere; it is your file.");
    txtMulti("##dosnotes", S.notes, ImVec2(-1, 70 * gUi));
    if (ImGui::IsItemDeactivatedAfterEdit()) { db.setNotes(icao, S.notes); say("notes saved"); }
    ImGui::SetNextItemWidth(320 * gUi);
    txt("Tags", "local, watch, interesting", S.tags);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::vector<std::string> t;
        std::string cur;
        for (char c : S.tags + ",") {
            if (c == ',') { while (!cur.empty() && cur.front() == ' ') cur.erase(0, 1); while (!cur.empty() && cur.back() == ' ') cur.pop_back(); if (!cur.empty()) t.push_back(cur); cur.clear(); }
            else cur += c;
        }
        db.setTags(icao, t);
        say("tags saved");
    }
    // facts the user researched elsewhere
    const auto fs2 = db.facts(icao);
    if (!fs2.empty() && ImGui::BeginTable("##dosfk", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("What");
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const auto& x : fs2) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(x.key.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(x.value.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(("x##" + x.key).c_str())) db.setFact(icao, x.key, "");
        }
        ImGui::EndTable();
    }
    ImGui::SetNextItemWidth(130 * gUi);
    ImGui::InputTextWithHint("##fk", "built, owner, ...", S.factKey, sizeof S.factKey);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputTextWithHint("##fv", "what you found out", S.factVal, sizeof S.factVal);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add fact") && S.factKey[0]) {
        db.setFact(icao, S.factKey, S.factVal);
        S.factKey[0] = 0;
        S.factVal[0] = 0;
        say("fact added");
    }
    // pictures the user attached
    ImGui::Spacing();
    if (!r.pictures.empty()) {
        ImGui::TextDisabled("Pictures");
        for (size_t i = 0; i < r.pictures.size(); i++) {
            const std::string& path = r.pictures[i];
            const Thumb* th = thumbFor(path);
            if (th && th->tex) {
                ImGui::Image(th->tex->texture(), ImVec2((float)th->w * gUi, (float)th->h * gUi));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
                ImGui::SameLine();
            } else if (th) {
                ImGui::TextDisabled("(not readable)");
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::TextUnformatted(path.c_str());
            if (ImGui::SmallButton(("Remove##p" + std::to_string(i)).c_str())) {
                std::vector<std::string> p = r.pictures;
                p.erase(p.begin() + (ptrdiff_t)i);
                db.setPictures(icao, p);
                forgetThumb(path);
                ImGui::EndGroup();
                break;
            }
            ImGui::EndGroup();
        }
    }
    if (ImGui::SmallButton("Attach a picture...")) {
        const std::string p = openFileDialog();
        if (!p.empty()) {
            std::vector<std::string> v = r.pictures;
            v.push_back(p);
            db.setPictures(icao, v);
            say("picture attached");
        }
    }
    // the timeline
    const auto ev = db.events(one);
    if (!ev.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Notable");
        for (const auto& x : ev) {
            ImGui::Text("%s  %-10s %s", hhmm(x.t).c_str(), x.kind.c_str(), x.text.c_str());
            if (x.user) {
                ImGui::SameLine();
                if (ImGui::SmallButton(("remove##e" + std::to_string(x.id)).c_str())) db.removeEvent(x.id);
            }
        }
    }
    ImGui::SetNextItemWidth(320 * gUi);
    ImGui::InputTextWithHint("##evn", "something you saw it do", S.note, sizeof S.note);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add to the timeline") && S.note[0]) {
        fl::Event e;
        e.icao = icao;
        e.t = (double)std::time(nullptr);
        e.kind = "special";
        e.text = S.note;
        e.user = true;
        db.addEvent(e);
        S.note[0] = 0;
        say("added");
    }
}

void aircraftTab(App& a) {
    filterRow();
    fl::Db& db = fl::global();
    fl::Filter f = currentFilter();
    f.limit = 300;
    const auto list = db.aircraftList(f);
    const float lw = std::min(300 * gUi, ImGui::GetContentRegionAvail().x * 0.35f);
    if (ImGui::BeginChild("##aclist", ImVec2(lw, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("##actab", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Aircraft");
            ImGui::TableSetupColumn("Seen");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableHeadersRow();
            for (const auto& r : list) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char lbl[64];
                snprintf(lbl, sizeof lbl, "%s##a%u", nameOf(r.icao, r.registration).c_str(), r.icao);
                if (ImGui::Selectable(lbl, S.acSel == r.icao, ImGuiSelectableFlags_SpanAllColumns)) S.acSel = r.icao;
                ImGui::TableNextColumn(); ImGui::Text("%lld", (long long)r.flightCount);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", fl::operationKindName(r.effectiveKind()));
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##acdet", ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        if (S.acSel) dossier(a, S.acSel);
        else ImGui::TextDisabled("pick an aircraft on the left");
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- patterns

void patterns(App& a) {
    filterRow();
    fl::Db& db = fl::global();
    const fl::Filter f = currentFilter();
    if (ImGui::BeginTable("##patt", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        barRow("Flights by hour of the day", db.hourHistogram(f), 24, nullptr);
        ImGui::TableNextColumn();
        barRow("Flights by day of the week", db.weekdayHistogram(f), 7, kDayNames);
        ImGui::EndTable();
    }
    ImGui::Separator();
    // how each kind of flying compares with the weeks before
    ImGui::TextUnformatted("Compared with the weeks before");
    if (ImGui::BeginTable("##pattrate", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Last day");
        ImGui::TableSetupColumn("Usually");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int k = 1; k < fl::OpKindCount; k++) {
            fl::Filter g = f;
            g.kind = k;
            g.since = 0;
            const fl::Ranked r = db.rateVsUsual(g, 86400, 6);
            if (r.count == 0 && r.seconds <= 0) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fl::operationKindName((fl::OperationKind)k));
            ImGui::TableNextColumn(); ImGui::Text("%lld", (long long)r.count);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.seconds);
            ImGui::TableNextColumn();
            if (r.seconds <= 0) ImGui::TextColored(pal::warnAmber(), "none in the weeks before");
            else if (r.value > 1.5) ImGui::TextColored(pal::warnAmber(), "more than usual (x%.1f)", r.value);
            else if (r.value < 0.5) ImGui::TextDisabled("quieter than usual");
            else ImGui::TextDisabled("about usual");
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (ImGui::BeginTable("##patt2", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Routes most flown");
        rankTable("##pr", "Route", db.topRoutes(f, 12), false);
        ImGui::Spacing();
        ImGui::TextUnformatted("Aircraft types");
        rankTable("##pt", "Type", db.topTypes(f, 10), false);
        ImGui::TableNextColumn();
        // where the flights of each interesting kind go and come from
        for (int k : {(int)fl::OpMedical, (int)fl::OpMilitary, (int)fl::OpSkydive, (int)fl::OpSurvey}) {
            const auto to = db.kindAirports(f, k, true, 6);
            const auto from = db.kindAirports(f, k, false, 6);
            if (to.empty() && from.empty()) continue;
            ImGui::Text("%s flights", fl::operationKindName((fl::OperationKind)k));
            if (!from.empty()) {
                ImGui::TextDisabled("  from:");
                for (const auto& r : from) { ImGui::SameLine(); ImGui::Text("%s (%lld)", r.key.c_str(), (long long)r.count); }
            }
            if (!to.empty()) {
                ImGui::TextDisabled("  to:");
                for (const auto& r : to) { ImGui::SameLine(); ImGui::Text("%s (%lld)", r.key.c_str(), (long long)r.count); }
            }
            ImGui::Spacing();
        }
        ImGui::EndTable();
    }
}

} // namespace

// ---------------------------------------------------------------- the window

void flightsWindow(App& a) {
    if (a.rxId != 1) {   // one history for the program: RX 1 owns it
        if (App* f = receiverById(1)) { f->flightsOpen = a.flightsOpen; flightsWindow(*f); a.flightsOpen = f->flightsOpen; }
        return;
    }
    if (!S.started) start();
    gThumbThisFrame = false;
    // the recorder runs whichever screen the user is on, and whether or not this window is open
    if (fl::global().isOpen()) {
        fl::Recorder& r = fl::globalRecorder();
        if (a.rx.standard == 12 && a.engine.running()) {   // ADS-B is the active mode
            if (a.rx.adsb.refLat || a.rx.adsb.refLon) r.setReceiver(a.rx.adsb.refLat, a.rx.adsb.refLon);
            r.feed(a.rx.adsb, (double)std::time(nullptr));
        }
    }
    if (!a.flightsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(960 * gUi, 620 * gUi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("Flight history"), &a.flightsOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (!S.msg.empty() && ImGui::GetTime() - S.msgT < 6) { ImGui::TextDisabled("%s", S.msg.c_str()); }
    if (ImGui::BeginTabBar("##fltabs")) {
        static const char* names[] = {"Overview", "Flights", "Aircraft", "Patterns"};
        for (int i = 0; i < 4; i++) {
            const bool want = S.tabWant == i;
            if (ImGui::BeginTabItem(tr(names[i]), nullptr, want ? ImGuiTabItemFlags_SetSelected : 0)) {
                S.tab = i;
                if (S.tabWant == i) S.tabWant = -1;
                if (i == 0) overview(a);
                else if (i == 1) flightsTab(a);
                else if (i == 2) aircraftTab(a);
                else patterns(a);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
