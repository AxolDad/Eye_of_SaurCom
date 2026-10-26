// DAB: the Map tab (the transmitters). The receiver names the transmitters of the network it hears by their TII codes (MainId / SubId, EN 300 401
// clause 14.8); with a list of where each transmitter of an ensemble stands they go on the map, with the distance and bearing from the
// antenna. The list is the user's own file in Eye of SaurCom's data folder (dab-transmitters.csv), with the positions the ensemble itself
// broadcasts (FIG 0/22) where the file has none:
//   eid,main,sub,lat,lon,site,power_kw,channel_mhz[,reports,first_seen,last_seen]      (eid in hex; a line starting with # is a comment)
// TII codes are unique within an ensemble, not worldwide: a transmitter is found by (ensemble id, MainId, SubId).
#include "dect2/common/thread_name.h"
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/engine/updater.h"
#include "dect2/dab/dab_fig.h"
#include <algorithm>
#include <atomic>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

namespace {

struct TxSite { double lat = 0, lon = 0, powerKw = 0, channelMhz = 0; std::string name; bool own = false; };

struct Heard { int eid = 0, mainId = 0, subId = 0; std::string ens; double mhz = 0; float levelDb = 0; double seen = 0; };

struct State {
    bool loaded = false;
    std::map<uint64_t, TxSite> sites;   // key: eid << 16 | main << 8 | sub
    std::string ownFile, loadMsg;
    adsbmap::View map;
    bool fitted = false;
    double refLat = 0, refLon = 0;
    bool haveRef = false;
    // the positions the ensemble itself broadcasts (FIG 0/22), refreshed every couple of seconds: key as in sites, the own file wins
    std::map<uint64_t, TxSite> air;
    double airAt = -10, heardAt = -10;
    // every transmitter heard since the program started, over all channels (a band scan fills it): key as in sites
    std::map<uint64_t, Heard> heard;
    std::string exportMsg;
};
State S;

uint64_t key(int eid, int mainId, int subId) { return ((uint64_t)(eid & 0xFFFF) << 16) | ((uint64_t)(mainId & 0xFF) << 8) | (uint64_t)(subId & 0xFF); }
std::filesystem::path u8(const std::string& p) { return std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())); }


std::vector<std::string> splitCsv(const std::string& line) {   // commas (or tabs: a TSV works too); a field in double quotes may hold commas
    std::vector<std::string> f;
    const char sep = line.find('\t') != std::string::npos ? '\t' : ',';
    std::string cur;
    bool q = false;
    for (char c : line) {
        if (c == '"') q = !q;
        else if (c == sep && !q) { f.push_back(cur); cur.clear(); }
        else cur += c;
    }
    f.push_back(cur);
    for (auto& s : f) { while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back(); while (!s.empty() && s.front() == ' ') s.erase(0, 1); }
    return f;
}

// reads one list into S.sites; returns the transmitters read (bad lines counted in `bad`)
int readList(const std::string& path, bool own, int& bad) {
    std::ifstream in(u8(path));
    if (!in) return -1;
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = splitCsv(line);
        if (!f.empty() && f[0] == "eid") continue;
        if (f.size() < 5) { bad++; continue; }
        if (f[3].empty() && f[4].empty()) continue;   // a line without a position (a submission form row): nothing to place
        char* e = nullptr;
        const long eid = std::strtol(f[0].c_str(), &e, 16);
        const int m = std::atoi(f[1].c_str()), sb = std::atoi(f[2].c_str());
        TxSite s;
        s.lat = std::atof(f[3].c_str()); s.lon = std::atof(f[4].c_str());
        if (f.size() > 5) s.name = f[5];
        if (f.size() > 6) s.powerKw = std::atof(f[6].c_str());
        if (f.size() > 7) s.channelMhz = std::atof(f[7].c_str());
        s.own = own;
        if (e == f[0].c_str() || m < 0 || m > 69 || sb < 0 || sb > 23 || std::fabs(s.lat) > 90 || std::fabs(s.lon) > 180) { bad++; continue; }
        S.sites[key((int)eid, m, sb)] = s;
        n++;
    }
    return n;
}

void load() {
    S.loaded = true;
    S.sites.clear();
    int bad = 0;
    S.ownFile = plat::dataDir() + "/dab-transmitters.csv";
    const int own = readList(S.ownFile, true, bad);
    char b[200];
    snprintf(b, sizeof b, "%d transmitter(s) in your list%s", (int)S.sites.size(), bad ? ", some lines not understood" : "");
    S.loadMsg = own < 0 ? std::string("no transmitter list yet: add your own file") : std::string(b);
}

// great-circle distance (km) and initial bearing (degrees from north)
void distBearing(double la1, double lo1, double la2, double lo2, double& km, double& deg) {
    const double r = M_PI / 180.0, p1 = la1 * r, p2 = la2 * r, dl = (lo2 - lo1) * r;
    const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    km = 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    deg = std::fmod(std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) / r + 360.0, 360.0);
}

const TxSite* siteOf(const Heard& h) {   // the list first, then what the ensemble itself broadcasts (the sub identifier's own position, else the main one's)
    auto it = S.sites.find(key(h.eid, h.mainId, h.subId));
    if (it != S.sites.end()) return &it->second;
    auto b = S.air.find(key(h.eid, h.mainId, h.subId));
    if (b == S.air.end()) b = S.air.find(key(h.eid, h.mainId, 0));
    return b != S.air.end() ? &b->second : nullptr;
}

} // namespace

// Called every frame (also while the Map tab is not shown, so a band scan fills the list): notes the transmitters of the ensemble on the air.
void dabTiiCollect(App& a) {
    const double now = ImGui::GetTime();
    if (now - S.heardAt < 0.5) return;
    S.heardAt = now;
    const DabTelemetry& d = a.rx.dab;
    if (!(a.engine.running() && a.rx.standard == 3 && d.state == 2) || d.tii.empty()) return;
    const DabEnsemble ens = a.engine.dabEnsemble();
    if (!ens.valid) return;
    for (const auto& t : d.tii) {
        Heard& h = S.heard[key(ens.eid, t.mainId, t.subId)];
        h.eid = ens.eid; h.mainId = t.mainId; h.subId = t.subId; h.ens = ens.label; h.mhz = a.freqMhz; h.levelDb = t.levelDb; h.seen = now;
    }
    if (now - S.airAt > 2.0) {   // the positions the broadcast itself gives (FIG 0/22: main identifier, and sub identifiers as offsets from it)
        S.airAt = now;
        const DabFicInfo fi = dabFicInfo(a.engine.dabFic());
        for (const auto& m : fi.transmitters) { TxSite s; s.lat = m.lat; s.lon = m.lng; s.name = "broadcast position"; S.air[key(ens.eid, m.mainId, 0)] = s; }
        for (const auto& t : fi.subTransmitters) if (t.placed) { TxSite s; s.lat = t.lat; s.lon = t.lng; s.name = "broadcast position"; S.air[key(ens.eid, t.mainId, t.subId)] = s; }
    }
}

// the heard transmitters as a file in Eye of SaurCom's data folder: the list's columns (so it can go straight into dab-transmitters.csv or a submission) plus the ensemble and level
static void exportHeard() {
    const std::string path = plat::dataDir() + "/dab-heard-" + std::to_string((long long)time(nullptr)) + ".csv";
    std::ofstream o(u8(path));
    if (!o) { S.exportMsg = "could not write " + path; return; }
    o << "eid,main,sub,lat,lon,site,power_kw,channel_mhz,ensemble,level_db\n";
    for (const auto& kv : S.heard) {
        const Heard& h = kv.second;
        const TxSite* st = siteOf(h);
        char b[256];
        snprintf(b, sizeof b, "%04X,%d,%d,", h.eid & 0xFFFF, h.mainId, h.subId);
        o << b;
        if (st) { snprintf(b, sizeof b, "%.5f,%.5f,", st->lat, st->lon); o << b; } else o << ",,";
        o << '"' << (st && st->name != "broadcast position" ? st->name : "") << '"' << ',';
        if (st && st->powerKw > 0) o << st->powerKw;
        snprintf(b, sizeof b, ",%.3f,\"%s\",%.1f\n", h.mhz, h.ens.c_str(), h.levelDb);
        o << b;
    }
    S.exportMsg = "saved " + std::to_string(S.heard.size()) + " transmitter(s) to " + path + " (CSV: eid,main,sub,lat,lon,site,power_kw,channel_mhz,ensemble,level_db)";
}

void dabTransmittersTab(App& a) {
    if (!S.loaded) load();
    // the antenna position: the one the map modes share (set in the ADS-B tuner, or below)
    if (!S.haveRef) {
        S.refLat = plat::prefs().getD("adsbLat", 0); S.refLon = plat::prefs().getD("adsbLon", 0);
        S.haveRef = S.refLat != 0 || S.refLon != 0;
    }
    dabTiiCollect(a);
    const DabTelemetry& d = a.rx.dab;
    const bool on = a.engine.running() && d.state == 2;
    const double now = ImGui::GetTime();

    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("Every transmitter of a DAB network sends its own identification (TII: MainId / SubId) in the null symbol. Eye of SaurCom lists every one it hears, on every channel "
                          "(run a band scan to fill the list), and puts them on the map where a transmitter list or the ensemble itself (FIG 0/22) gives the position. Click a marker or a row to tune that ensemble.");
      ImGui::PopTextWrapPos(); }

    struct Row { const Heard* h; const TxSite* site; double km = -1, deg = 0; bool live; };
    std::vector<Row> rows;
    for (const auto& kv : S.heard) {
        Row r{&kv.second, siteOf(kv.second), -1, 0, now - kv.second.seen < 3.0};
        if (r.site && S.haveRef) distBearing(S.refLat, S.refLon, r.site->lat, r.site->lon, r.km, r.deg);
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.h->mhz != y.h->mhz ? x.h->mhz < y.h->mhz : x.h->levelDb > y.h->levelDb; });
    int located = 0;
    for (const auto& r : rows) if (r.site) located++;
    auto tuneTo = [&](const Heard& h) {
        a.freqMhz = h.mhz; a.tune.centerHz = h.mhz * 1e6;
        if (a.engine.running()) { a.engine.retuneReset(a.tune); a.peak.clear(); }
    };

    // the full-screen view: the map takes the whole height, the table and the controls stand beside it; otherwise the map is above them
    const bool full = adsbmap::fullActive();
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float sideW = full ? std::max(340.f * gUi, av.x * 0.36f) : av.x;
    if (located > 0 || S.haveRef) {
        const ImVec2 size(full ? av.x - sideW - 8 * gUi : av.x, full ? av.y : std::max(160.f * gUi, av.y * 0.55f));
        if (!S.fitted && located > 0) {   // centre on what was found, once
            double la = 0, lo = 0; int n = 0;
            for (const auto& r : rows) if (r.site) { la += r.site->lat; lo += r.site->lon; n++; }
            if (S.haveRef) { la += S.refLat; lo += S.refLon; n++; }
            S.map.lat = la / n; S.map.lon = lo / n; S.map.zoom = 8; S.fitted = true;
        } else if (!S.fitted && S.haveRef) { S.map.lat = S.refLat; S.map.lon = S.refLon; S.map.zoom = 8; }
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        adsbmap::draw(S.map, size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
        const ImVec2 me = adsbmap::project(S.refLat, S.refLon);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool inMap = ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + size.x, p0.y + size.y));
        const Row* hit = nullptr;
        float hitD = 12.f * gUi;
        for (const auto& r : rows) {
            if (!r.site) continue;
            const ImVec2 q = adsbmap::project(r.site->lat, r.site->lon);
            const float rel = std::max(0.f, std::min(1.f, r.h->levelDb / 40.f));   // brighter and larger for a stronger transmitter
            if (S.haveRef && r.live) dl->AddLine(me, q, IM_COL32(120, 200, 255, (int)(60 + 120 * rel)), 1.5f);
            dl->AddCircleFilled(q, (4.f + 4.f * rel) * gUi, r.live ? IM_COL32(255, 170, 60, (int)(140 + 115 * rel)) : IM_COL32(150, 170, 200, (int)(120 + 100 * rel)));
            char lab[160];
            snprintf(lab, sizeof lab, "%s  %d/%d  %.3f", r.site->name.empty() || r.site->name == "broadcast position" ? r.h->ens.c_str() : r.site->name.c_str(), r.h->mainId, r.h->subId, r.h->mhz);
            dl->AddText(ImVec2(q.x + 8 * gUi, q.y - 7 * gUi), IM_COL32(255, 255, 255, 230), lab);
            const float dd = std::hypot(mouse.x - q.x, mouse.y - q.y);
            if (inMap && dd < hitD) { hitD = dd; hit = &r; }
        }
        if (S.haveRef) {   // the antenna: a cross
            const float c = 6.f * gUi;
            dl->AddLine(ImVec2(me.x - c, me.y), ImVec2(me.x + c, me.y), IM_COL32(120, 200, 255, 255), 2.f);
            dl->AddLine(ImVec2(me.x, me.y - c), ImVec2(me.x, me.y + c), IM_COL32(120, 200, 255, 255), 2.f);
        }
        dl->PopClipRect();
        if (hit) {
            char dist[32] = "";
            if (hit->km >= 0) snprintf(dist, sizeof dist, ", %.0f km", hit->km);
            ImGui::SetTooltip("%s  %.3f MHz\nTII %d / %d, %.0f dB above the noise%s\nclick to tune this ensemble", hit->h->ens.c_str(), hit->h->mhz, hit->h->mainId, hit->h->subId,
                              hit->h->levelDb, dist);
            if (ImGui::IsMouseReleased(0) && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 16.f) tuneTo(*hit->h);
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 6 * gUi, p0.y + 6 * gUi));
        if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::Button("fit")) S.fitted = false;
        ImGui::SameLine(0, 8 * gUi);
        adsbmap::tileControls("never the position itself");
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y - ImGui::GetTextLineHeightWithSpacing()));
        if (rows.empty()) adsbmap::legend(size.x, on ? "listening for transmitter identification..." : "start the receiver on a DAB channel");
        else if (located == 0) adsbmap::legend(size.x, "none of the transmitters heard has a known position yet (see the list)");
        if (full) { ImGui::SetCursorScreenPos(ImVec2(p0.x + size.x + 8 * gUi, p0.y)); ImGui::BeginChild("##tiiside", ImVec2(sideW, av.y), false); }
        else ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y + 4 * gUi));
    } else if (full) ImGui::BeginChild("##tiiside", ImVec2(sideW, av.y), false);

    // ---- the table: every transmitter heard (ids, ensemble, channel, level), with the position where one is known
    if (rows.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", !a.engine.running() ? "start the receiver on a DAB channel, or run a band scan on the Scan tab" :
                            d.tiiFrames < 30 ? "listening for transmitter identification..." : "no transmitter identification heard (yet): TII needs a decent signal and is not sent by every network");
        ImGui::PopTextWrapPos();
    } else if (ImGui::BeginTable("##dabtii", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX,
                               ImVec2(0, std::max(5 * ImGui::GetTextLineHeightWithSpacing(), ImGui::GetContentRegionAvail().y - 4 * ImGui::GetTextLineHeightWithSpacing())))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("TII"); ImGui::TableSetupColumn("Ensemble"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Transmitter", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Distance"); ImGui::TableSetupColumn("Bearing"); ImGui::TableSetupColumn("Power");
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            const Heard& h = *r.h;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)key(h.eid, h.mainId, h.subId));
            char id[24];
            snprintf(id, sizeof id, "%02d / %02d", h.mainId, h.subId);
            ImGui::PushFont(a.mono, 0);
            if (ImGui::Selectable(id, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) tuneTo(h);
            ImGui::PopFont();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("MainId %d (pattern), SubId %d (comb)\n%.1f dB above the noise of the null symbol\nclick to tune %s", h.mainId, h.subId, h.levelDb, h.ens.c_str());
            ImGui::TableNextColumn(); if (r.live) ImGui::TextUnformatted(h.ens.c_str()); else ImGui::TextDisabled("%s", h.ens.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f", h.mhz);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", h.levelDb);
            ImGui::TableNextColumn();
            if (r.site) { ImGui::TextUnformatted(r.site->name.empty() ? "?" : r.site->name.c_str());
                if (ImGui::IsItemHovered()) { if (r.site->own) ImGui::SetTooltip("from your own file"); else if (r.site->name == "broadcast position") ImGui::SetTooltip("%.4f, %.4f\nsent by the ensemble itself (FIG 0/22), no site name", r.site->lat, r.site->lon); } }
            else {
                ImGui::TextDisabled("?");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Position not known yet: add it to your own list (%s).", S.ownFile.c_str());
            }
            ImGui::PopID();
            ImGui::TableNextColumn(); if (r.km >= 0) ImGui::Text("%.0f km", r.km); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.km >= 0) ImGui::Text("%.0f°", r.deg); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.site && r.site->powerKw > 0) ImGui::Text("%.1f kW", r.site->powerKw); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }

    // ---- the list and the antenna position
    ImGui::Spacing();
    ImGui::TextDisabled("%s", S.loadMsg.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your list: %s\n(columns eid,main,sub,lat,lon,site,power_kw,channel_mhz; eid in hex; # starts a comment)", S.ownFile.c_str());
    flowNext(10 * gUi);   // the row wraps in a narrow window
    if (ImGui::SmallButton("reload")) { load(); S.fitted = false; }
    flowNext(6 * gUi);
    if (ImGui::SmallButton("open folder")) dect2::openUrl(plat::dataDir());
    flowNext(6 * gUi);
    ImGui::BeginDisabled(S.heard.empty());
    if (ImGui::SmallButton("export heard")) exportHeard();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Save every transmitter heard (all channels) as a CSV file in Eye of SaurCom's data folder, to add to your list");
    flowNext(6 * gUi);
    if (ImGui::SmallButton("clear heard")) S.heard.clear();
    flowEnd();
    if (!S.exportMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.exportMsg.c_str()); ImGui::PopTextWrapPos(); }
    if (!S.haveRef) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Distances and bearings: set the antenna position in the ADS-B tuner."); ImGui::PopTextWrapPos(); }
    if (full) ImGui::EndChild();
}
