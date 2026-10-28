// The Eye (Tools > The Eye, Ctrl+E): everything the station decodes, across modes, in one window. A chip per kind of contact with its live count,
// a scope that places every contact with a position by bearing and distance from the receiver, and one timeline of first sightings and
// messages. The model is core/src/eye/eye.cpp, fed from the automation engine's overview hook from the start (so the window shows what
// was heard before it was opened). The window copies what it shows out of the model at most four times a second, and only when the model
// changed, so a busy ADS-B receiver does not make it stutter or hold up the decoders.
#include "../app.h"
#include "dect2/automation/engine.h"
#include "dect2/df/xfix.h"
#include "dect2/eye/eye.h"
#include "dect2/track/track_geo.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

namespace {

namespace ey = dect2::eye;

struct State {
    bool hooked = false;
    uint32_t mask = 0;            // groups shown (0 = all)
    int window = 1;               // index into kWindows
    int range = 0;                // index into kRanges (0 = automatic)
    char filter[96] = "";
    bool paused = false;
    double pausedAt = 0;          // the timeline shows nothing newer than this while paused
    std::string selected;         // group|ident of the contact picked on the scope or in the timeline
    bool scrollToSel = false;     // a dot was clicked: bring its newest row into view
    bool clearAsk = false;        // Clear asks first, as Messages does
    // what was last copied out of the model, and for which settings
    uint64_t version = ~0ull;
    double builtAt = -1;
    uint32_t builtMask = ~0u;
    int builtWindow = -1;
    std::string builtFilter;
    std::array<ey::GroupStat, ey::kGroups> stats{};
    std::vector<ey::Contact> contacts;
    std::vector<ey::Row> rows;
};
State S;

constexpr double kWindows[] = {300, 900, 3600};
const char* const kWindowNames[] = {"5 min", "15 min", "1 hour"};
constexpr double kRanges[] = {0, 10, 50, 200, 500, 2000};
const char* const kRangeNames[] = {"Auto", "10 km", "50 km", "200 km", "500 km", "2000 km"};

ImU32 groupColour(int g, int alpha = 255) {
    static const unsigned char c[ey::kGroups][3] = {
        {255, 179, 71},   // aircraft: amber
        {255, 216, 102},  // air data: yellow
        {79, 184, 209},   // ships: cyan
        {123, 216, 143},  // APRS: green
        {199, 146, 234},  // sondes: violet
        {90, 212, 166},   // mesh: teal
        {255, 107, 107},  // pagers: red
        {106, 156, 255},  // Bluetooth: blue
        {247, 140, 108},  // drones: coral
        {255, 255, 255},  // located (cross-site fixes): white
    };
    const int i = g >= 0 && g < ey::kGroups ? g : 0;
    return IM_COL32(c[i][0], c[i][1], c[i][2], alpha);
}

// a contact's mark: the kind is in the shape as well as the colour (amber aircraft, red pagers and coral drones are close for colour-blind eyes)
void mark(ImDrawList* dl, ImVec2 p, float s, int group, ImU32 col) {
    switch (group % 3) {
    case 0: dl->AddCircleFilled(p, s, col); break;
    case 1: dl->AddRectFilled(ImVec2(p.x - s * 0.9f, p.y - s * 0.9f), ImVec2(p.x + s * 0.9f, p.y + s * 0.9f), col); break;
    default: dl->AddTriangleFilled(ImVec2(p.x, p.y - s * 1.2f), ImVec2(p.x - s * 1.1f, p.y + s * 0.9f), ImVec2(p.x + s * 1.1f, p.y + s * 0.9f), col); break;
    }
}

std::string key(int group, const std::string& ident) { return std::to_string(group) + "|" + ident; }

std::string clock(double t, bool utc = false) {
    const time_t s = (time_t)t;
    struct tm l;
#ifdef _WIN32
    if (utc) gmtime_s(&l, &s); else localtime_s(&l, &s);
#else
    if (utc) gmtime_r(&s, &l); else localtime_r(&s, &l);
#endif
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", &l);
    return b;
}

// a tooltip that wraps: pager, ACARS and APRS texts can be hundreds of characters on one line
void wrappedTip(const std::string& s) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35);
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// SameLine() when an item of width w still fits on this line, else let it start the next one (narrow windows)
void sameLineIfFits(float w) {
    const float x = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w;
    if (x < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
}

std::string ago(double sec) {
    char b[32];
    if (sec < 60) snprintf(b, sizeof b, "%.0f s ago", std::max(0.0, sec));
    else if (sec < 3600) snprintf(b, sizeof b, "%.0f min ago", sec / 60);
    else snprintf(b, sizeof b, "%.1f h ago", sec / 3600);
    return b;
}

std::string who(const std::string& label, const std::string& ident) {
    if (label.empty() || label == ident) return ident;
    return label + "  (" + ident + ")";
}

// a round number of km for the outer ring
double niceRange(double km) {
    for (double r : {1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 300.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0})
        if (km <= r) return r;
    return 20000;
}

// copies the model's state into S when it changed (or the settings did), at most four times a second
void rebuild(double now) {
    ey::Overview& o = ey::global();
    const uint64_t v = o.version();
    const double t = ImGui::GetTime();
    const bool settings = S.mask != S.builtMask || S.window != S.builtWindow || S.builtFilter != S.filter;
    if (!settings && (t - S.builtAt < 0.25 || (v == S.version && t - S.builtAt < 1.0))) return;   // once a second regardless: ages and fading move
    S.version = v; S.builtAt = t; S.builtMask = S.mask; S.builtWindow = S.window; S.builtFilter = S.filter;
    const double win = kWindows[S.window];
    S.stats = o.stats(now, win);
    S.contacts = o.contacts(now, win, S.mask, S.filter);
    if (S.paused && !settings) return;   // paused: the rows being read stay, however much arrives meanwhile
    S.rows = o.timeline(1000, S.mask, S.filter);
    if (S.paused) S.rows.erase(std::remove_if(S.rows.begin(), S.rows.end(), [](const ey::Row& r) { return r.time > S.pausedAt; }), S.rows.end());
}

const ey::Contact* findContact(const std::string& k) {
    for (const auto& c : S.contacts) if (key(c.group, c.ident) == k) return &c;
    return nullptr;
}

// what can be done with one contact: the same items in the scope's and the timeline's right-click menus and under the selection
void contactActions(App& a, int group, const std::string& ident, bool hasPos, double lat, double lon, bool buttons) {
    auto item = [buttons](const char* label) { return buttons ? ImGui::SmallButton(label) : ImGui::MenuItem(label); };
    if (hasPos) {
        if (item(tr("Show on map"))) mapShow(a, lat, lon);
        if (buttons) ImGui::SameLine();
    }
    if (item(tr("Find in Messages"))) msgdbFind(a, ident);
    if (buttons) ImGui::SameLine();
    if (item(tr("Copy identity"))) ImGui::SetClipboardText(ident.c_str());
    (void)group;
}

void chips() {
    for (int g = 0; g < ey::kGroups; g++) {
        const bool on = S.mask == 0 || (S.mask >> g & 1);
        char lab[64];
        snprintf(lab, sizeof lab, "%s  %d###chip%d", tr(ey::groupName(g)), S.stats[(size_t)g].active, g);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? groupColour(g, 70) : IM_COL32(60, 60, 60, 60));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, groupColour(g, 120));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled));
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const ImVec2 ls = ImGui::CalcTextSize(lab, nullptr, true);
        const float mw = 12 * gUi;   // room for the mark, the legend of the scope's shapes
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(1.f, 0.5f));
        if (ImGui::Button(lab, ImVec2(ls.x + mw + 2 * ImGui::GetStyle().FramePadding.x, 0))) S.mask = S.mask == (1u << g) ? 0 : 1u << g;   // only this kind; a second click shows all again
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        mark(ImGui::GetWindowDrawList(), ImVec2(bp.x + ImGui::GetStyle().FramePadding.x + 4 * gUi, bp.y + ImGui::GetFrameHeight() / 2), 3.5f * gUi, g, groupColour(g, on ? 255 : 110));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", (std::to_string(S.stats[(size_t)g].active) + " " + tr("heard in the window") + ", " + std::to_string(S.stats[(size_t)g].events) + " " +
                                     tr("events") + "\n" + tr("Click to show only these; click again to show all")).c_str());
        if (g + 1 < ey::kGroups) {
            char next[64];
            snprintf(next, sizeof next, "%s  %d", tr(ey::groupName(g + 1)), S.stats[(size_t)g + 1].active);
            sameLineIfFits(ImGui::CalcTextSize(next).x + 12 * gUi + 2 * ImGui::GetStyle().FramePadding.x);
        }
    }
}

void scope(App& a, double now, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##eyescope", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 c(p0.x + size / 2, p0.y + size / 2);
    const float R = size / 2 - 14 * gUi;
    const double win = kWindows[S.window];

    // the iris: a dark ember glow, darker towards the rim
    for (int i = 8; i >= 1; i--) {
        const float f = i / 8.f;
        dl->AddCircleFilled(c, R * f, IM_COL32((int)(70 - 40 * f), (int)(16 - 8 * f), 8, 255), 64);
    }
    const bool haveHome = receiverLocationSet();
    double hLat = 0, hLon = 0;
    if (haveHome) receiverLocation(hLat, hLon);

    // contacts whose position is recent enough for the window, as (bearing, distance)
    struct Dot { const ey::Contact* c; double brg, km; };
    std::vector<Dot> dots;
    std::vector<double> dist;
    int stale = 0;   // heard in the window, but the last position is older than it
    if (haveHome)
        for (const auto& k : S.contacts) {
            if (!k.hasPos) continue;
            if (k.posTime < now - win) { stale++; continue; }
            const double km = dect2::track::gcDistM(hLat, hLon, k.lat, k.lon) / 1000.0;
            dots.push_back({&k, dect2::track::gcBearingDeg(hLat, hLon, k.lat, k.lon), km});
            dist.push_back(km);
        }
    // Auto: the ring that holds 95 % of them, so one far or wrong position does not shrink everything else to the middle
    double rangeKm = kRanges[S.range];
    if (rangeKm <= 0) {
        double ref = 1;
        if (!dist.empty()) {
            std::sort(dist.begin(), dist.end());
            ref = dist[std::min(dist.size() - 1, (size_t)std::ceil(dist.size() * 0.95) - 1)];
        }
        rangeKm = niceRange(std::max(1.0, ref * 1.05));
    }

    // rings, cross and the compass points
    const ImU32 ring = IM_COL32(255, 140, 40, 70);
    for (int i = 1; i <= 4; i++) dl->AddCircle(c, R * i / 4.f, ring, 64, 1.f);
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), ring);
    dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), ring);
    const ImU32 txt = IM_COL32(255, 196, 120, 200);
    const float th = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(c.x - 4 * gUi, c.y - R - th - 1), txt, "N");
    dl->AddText(ImVec2(c.x + R + 3 * gUi, c.y - th / 2), txt, "E");
    dl->AddText(ImVec2(c.x - 4 * gUi, c.y + R + 1), txt, "S");
    dl->AddText(ImVec2(c.x - R - 12 * gUi, c.y - th / 2), txt, "W");
    char rl[32];
    snprintf(rl, sizeof rl, rangeKm >= 10 ? "%.0f km" : "%.1f km", rangeKm);
    dl->AddText(ImVec2(c.x + 3 * gUi, c.y - R + 2), IM_COL32(255, 196, 120, 140), rl);

    // the sweep: a slow turn with a fading tail
    const double a0 = std::fmod(ImGui::GetTime() * 0.6, 2 * M_PI);
    for (int i = 0; i < 24; i++) {
        const double ang = a0 - i * 0.025;
        const ImVec2 e(c.x + (float)(std::sin(ang) * R), c.y - (float)(std::cos(ang) * R));
        dl->AddLine(c, e, IM_COL32(255, 170, 60, (int)(90 * (1 - i / 24.0))), 2.f * gUi);
    }

    // the dots: bright when the position is new, fading over the window
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const Dot* hot = nullptr;
    float hotD = 8 * gUi;
    int drawn = 0, beyond = 0;
    for (const Dot& d : dots) {
        if (d.km > rangeKm) { beyond++; continue; }
        drawn++;
        const float r = (float)(d.km / rangeKm) * R;
        const ImVec2 p(c.x + (float)std::sin(d.brg * M_PI / 180) * r, c.y - (float)std::cos(d.brg * M_PI / 180) * r);
        const int alpha = (int)std::clamp(255.0 - 185.0 * (now - d.c->posTime) / win, 70.0, 255.0);
        const bool sel = key(d.c->group, d.c->ident) == S.selected;
        mark(dl, p, (sel ? 5.f : 3.5f) * gUi, d.c->group, groupColour(d.c->group, alpha));
        if (sel) dl->AddCircle(p, 9 * gUi, IM_COL32(255, 240, 200, 230), 0, 1.5f);
        const float dd = std::hypot(mouse.x - p.x, mouse.y - p.y);
        if (hovered && dd < hotD) { hotD = dd; hot = &d; }
    }

    // the pupil: the receiver, a slit like the icon's
    dl->AddEllipseFilled(c, ImVec2(3 * gUi, 11 * gUi), IM_COL32(10, 4, 4, 255));
    dl->AddEllipse(c, ImVec2(5 * gUi, 13 * gUi), IM_COL32(255, 236, 170, 160));

    if (!haveHome) {
        const char* m = tr("No location set");
        const ImVec2 ts = ImGui::CalcTextSize(m);
        if (ts.x < 2 * R) dl->AddText(ImVec2(c.x - ts.x / 2, c.y + R / 2), IM_COL32(255, 220, 180, 230), m);
    }
    if (hot) {
        const ey::Contact& k = *hot->c;
        char b[96];
        snprintf(b, sizeof b, "%.1f km, %03.0f\xC2\xB0", hot->km, hot->brg);
        wrappedTip(who(k.label, k.ident) + "\n" + tr(ey::groupName(k.group)) + ", " + k.mode + "\n" + b + ", " + tr("position") + " " + ago(now - k.posTime) +
                   (k.text.empty() ? std::string() : "\n" + k.text) + "\n" + tr("Click to pick it out, right-click for more"));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { S.selected = key(k.group, k.ident); S.scrollToSel = true; }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { S.selected = key(k.group, k.ident); ImGui::OpenPopup("##eyedot"); }
    } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        S.selected.clear();
    }
    if (ImGui::BeginPopup("##eyedot")) {
        if (const ey::Contact* k = findContact(S.selected)) contactActions(a, k->group, k->ident, k->hasPos, k->lat, k->lon, false);
        else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    int withPos = 0;
    for (const auto& k : S.contacts) if (k.hasPos) withPos++;
    std::string line = haveHome ? std::to_string(drawn) + " " + tr("on the scope")
                                : std::to_string(withPos) + " " + tr("with a position: set your location to see them");
    if (beyond) line += ", " + std::to_string(beyond) + " " + tr("beyond the range");
    if (stale) line += ", " + std::to_string(stale) + " " + tr("with an old position");
    int noPos = 0;
    for (const auto& k : S.contacts) if (!k.hasPos) noPos++;
    if (noPos) line += ", " + std::to_string(noPos) + " " + tr("without a position");
    ImGui::TextDisabled("%s", line.c_str());
    if (!haveHome && ImGui::SmallButton(tr("Set my location on the map..."))) a.mapOpen = true;
}

void timeline(App& a, double now) {
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##eyetl", 4, fl)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time (local)"), ImGuiTableColumnFlags_WidthFixed,
                            std::max(ImGui::CalcTextSize("00:00:00").x, ImGui::CalcTextSize(tr("Time (local)")).x) + 12 * gUi);
    ImGui::TableSetupColumn(tr("Mode"), ImGuiTableColumnFlags_WidthFixed, 96 * gUi);
    ImGui::TableSetupColumn(tr("Identity"), ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn(tr("What"), ImGuiTableColumnFlags_WidthStretch, 2.f);
    ImGui::TableHeadersRow();
    if (S.scrollToSel) {   // the newest row of the contact picked on the scope
        S.scrollToSel = false;
        for (size_t i = 0; i < S.rows.size(); i++)
            if (key(S.rows[i].group, S.rows[i].ident) == S.selected) { ImGui::SetScrollY(i * ImGui::GetTextLineHeightWithSpacing()); break; }
    }
    ImGuiListClipper clip;   // every row is one line: the clipper needs equal heights
    clip.Begin((int)S.rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const ey::Row& r = S.rows[(size_t)i];
            const std::string k = key(r.group, r.ident);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((void*)(uintptr_t)r.seq);   // not the position: rows move down as new ones arrive
            if (ImGui::Selectable(clock(r.time).c_str(), k == S.selected, ImGuiSelectableFlags_SpanAllColumns)) S.selected = k == S.selected ? std::string() : k;
            if (ImGui::IsItemHovered()) {
                std::string tip = ago(now - r.time) + ", " + clock(r.time, true) + " UTC";
                if (r.freqHz > 0) { char b[48]; snprintf(b, sizeof b, ", %.4f MHz", r.freqHz / 1e6); tip += b; }
                if (r.hasPos) { char b[64]; snprintf(b, sizeof b, "\n%.4f, %.4f", r.lat, r.lon); tip += b; }
                if (!r.first || r.text != "first seen") tip += "\n\n" + r.text;
                tip += std::string("\n\n") + tr("Right-click: show on map, find in Messages, copy");
                wrappedTip(tip);
            }
            if (ImGui::BeginPopupContextItem("##eyerow")) {
                S.selected = k;
                const ey::Contact* c = findContact(k);
                const bool hasPos = r.hasPos || (c && c->hasPos);
                contactActions(a, r.group, r.ident, hasPos, r.hasPos ? r.lat : c ? c->lat : 0, r.hasPos ? r.lon : c ? c->lon : 0, false);
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float th = ImGui::GetTextLineHeight();
            mark(ImGui::GetWindowDrawList(), ImVec2(p.x + 5 * gUi, p.y + th / 2), 4 * gUi, r.group, groupColour(r.group));
            ImGui::Dummy(ImVec2(12 * gUi, th));
            ImGui::SameLine();
            ImGui::TextUnformatted(r.mode.empty() ? tr(ey::groupName(r.group)) : r.mode.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(who(r.label, r.ident).c_str());
            ImGui::TableNextColumn();
            if (r.first && r.text == "first seen") ImGui::TextColored(ImVec4(1.f, 0.85f, 0.55f, 1.f), "%s", tr("first seen"));
            else if (r.first) ImGui::TextColored(ImVec4(1.f, 0.85f, 0.55f, 1.f), "%s", r.text.c_str());
            else ImGui::TextUnformatted(r.text.c_str());   // one line; the column clips it, the tooltip has it all
        }
    ImGui::EndTable();
}

}  // namespace

void eyeTick(App& a) {
    (void)a;
    if (S.hooked) return;
    S.hooked = true;
    // The Eye's hook also feeds the cross-site fixer: bearings (family signal) from several stations become located signals. Runs on decoder threads.
    dect2::automation::global().setOverview([](const dect2::automation::Event& e) {
        if (e.type == "df.bearing") {
            static dect2::df::CrossFixer fixer;   // has its own mutex
            fixer.add(e);
            for (const auto& f : fixer.fixes(e.time))
                ey::global().ingestLocated(f.time, f.freqHz, f.fix.lat, f.fix.lon, f.fix.e95.semiMajorM, f.sites);
            return;
        }
        ey::global().ingest(e);
    }, ey::familyMask() | (1u << dect2::automation::FSignal));
}

void eyeWindow(App& a) {
    ImGuiIO& io = ImGui::GetIO();
    if ((io.KeyCtrl || io.KeySuper) && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_E, false)) a.eyeOpen = !a.eyeOpen;
    if (!a.eyeOpen) return;
    ImGui::SetNextWindowSize(ImVec2(980 * gUi, 620 * gUi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("The Eye"), &a.eyeOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }

    const double now = dect2::automation::wallNow();
    ey::Overview& o = ey::global();
    rebuild(now);

    ImGui::TextColored(ImVec4(1.f, 0.72f, 0.36f, 1.f), "%s", tr("Everything this station is seeing, from every mode, in one place."));
    chips();

    // the labels go in front of the combos (ImGui puts them after, where "Window" reads as belonging to the next one)
    auto btnW = [](const char* s) { return ImGui::CalcTextSize(s).x + 2 * ImGui::GetStyle().FramePadding.x; };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Window"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::Combo("##eyewin", &S.window, kWindowNames, IM_ARRAYSIZE(kWindowNames));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How far back the counts and the scope look"));
    sameLineIfFits(ImGui::CalcTextSize(tr("Range")).x + 100 * gUi);
    ImGui::TextUnformatted(tr("Range"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::Combo("##eyerange", &S.range, kRangeNames, IM_ARRAYSIZE(kRangeNames));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The outer ring of the scope. Auto fits most of the contacts"));
    sameLineIfFits(200 * gUi);
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputTextWithHint("##eyefilter", tr("Filter: call sign, ship, text ..."), S.filter, sizeof S.filter);
    sameLineIfFits(btnW(tr("Pause")) + ImGui::GetFrameHeight());
    if (ImGui::Checkbox(tr("Pause"), &S.paused)) { S.pausedAt = now; S.builtAt = -1; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hold the timeline still to read it; the counts and the scope keep running"));
    if (S.clearAsk) {
        sameLineIfFits(btnW(tr("Really clear")) + btnW(tr("Keep")));
        if (ImGui::Button(tr("Really clear"))) { o.clear(); S.selected.clear(); S.builtAt = -1; S.clearAsk = false; }
        ImGui::SameLine();
        if (ImGui::Button(tr("Keep"))) S.clearAsk = false;
    } else {
        sameLineIfFits(btnW(tr("Clear...")));
        if (ImGui::Button(tr("Clear..."))) S.clearAsk = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Forget everything the Eye has seen so far (Messages keeps its copy)"));
    }
    sameLineIfFits(btnW(tr("Map")));
    if (ImGui::Button(tr("Map"))) a.mapOpen = true;
    sameLineIfFits(btnW(tr("Messages...")));
    if (ImGui::Button(tr("Messages..."))) a.msgdbOpen = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Search everything decoded so far, also what is older than this window"));

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) S.selected.clear();

    // the picked contact, and what can be done with it; the line is always there, so the layout does not jump when something is picked
    if (S.paused) {
        ImGui::TextColored(ImVec4(1.f, 0.72f, 0.36f, 1.f), "%s", tr("Paused"));
        ImGui::SameLine();
    }
    if (const ey::Contact* k = S.selected.empty() ? nullptr : findContact(S.selected)) {
        mark(ImGui::GetWindowDrawList(), ImVec2(ImGui::GetCursorScreenPos().x + 5 * gUi, ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() / 2),
             4 * gUi, k->group, groupColour(k->group));
        ImGui::Dummy(ImVec2(12 * gUi, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::Text("%s  ·  %s  ·  %s", who(k->label, k->ident).c_str(), k->mode.c_str(), ago(now - k->lastSeen).c_str());
        ImGui::SameLine();
        contactActions(a, k->group, k->ident, k->hasPos, k->lat, k->lon, true);
    } else {
        ImGui::TextDisabled("%s", tr("Click a dot or a line to pick it out; right-click for actions; Esc lets go"));
    }

    const float avail = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing();
    const float scopeSize = std::clamp(std::min(avail, ImGui::GetContentRegionAvail().x * 0.45f), 220 * gUi, 640 * gUi);
    const bool side = ImGui::GetContentRegionAvail().x - scopeSize > 360 * gUi;   // narrow: the timeline goes under the scope
    ImGui::BeginGroup();
    scope(a, now, scopeSize);
    ImGui::EndGroup();
    if (side) ImGui::SameLine();
    ImGui::BeginChild("##eyeright", ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, 160 * gUi)));
    if (o.total() == 0 && a.engine.running()) {
        ImGui::Dummy(ImVec2(0, 20 * gUi));
        ImGui::TextWrapped("%s", tr("Listening. Nothing decoded yet (or since the last Clear): what the running mode hears shows up here."));
    } else if (o.total() == 0) {
        ImGui::Dummy(ImVec2(0, 20 * gUi));
        ImGui::TextWrapped("%s", tr("Nothing seen yet. Start a mode that decodes something: ADS-B, AIS, APRS, pagers, LoRa and Meshtastic, "
                                     "radiosondes, Bluetooth, Remote ID or the aircraft data links. What it hears shows up here, on one timeline and "
                                     "on the scope, whichever mode heard it."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", tr("No radio? Pick a mode and press Start with the test signal as the source."));
    } else if (S.rows.empty()) {
        ImGui::Dummy(ImVec2(0, 20 * gUi));
        ImGui::TextDisabled("%s", tr("Nothing matches the filter or the chosen kind."));
        if (ImGui::SmallButton(tr("Show everything"))) { S.filter[0] = 0; S.mask = 0; }
    } else {
        timeline(a, now);
    }
    ImGui::EndChild();
    ImGui::End();
}
