// Tools > Deep Space Network: what NASA's DSN dishes are doing now, from the public DSN Now XML (eyes.nasa.gov, read only, nothing is
// sent), fetched every 5 s while the window is open. The configuration (spacecraft names, dish sizes, site positions) is fetched once and
// kept in the data folder for the next start. core/src/net/dsn.cpp and the Rust crate zorcom_dsn read the files.
#include "../app.h"
#include "dect2/net/dsn.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <future>
#include <sstream>
#include <string>

namespace {

struct Fetch { bool ok = false; std::string text; };

struct State {
    dect2::dsn::Names names;
    dect2::dsn::Report report;
    bool haveReport = false;
    std::future<Fetch> feed, config;
    double nextPoll = 0;          // seconds (steady clock)
    bool configTried = false;
    bool activeOnly = true;
    std::string status;
    int failures = 0;
};
State S;

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::string configPath() { return plat::dataDir() + "/dsn_config.xml"; }

Fetch get(std::string url, std::string path) {
    Fetch r;
    const std::string part = path + ".part";
    if (plat::fetchUrl(url, part, "EyeOfZorCom (DSN Now)")) {
        r.text = readFile(part);
        r.ok = !r.text.empty();
        std::remove(path.c_str());
        std::rename(part.c_str(), path.c_str());
    } else {
        std::remove(part.c_str());
    }
    return r;
}

void poll() {
    const double now = nowSec();
    if (!S.configTried) {
        S.configTried = true;
        const std::string cached = readFile(configPath());
        if (!cached.empty()) S.names.load(cached);
        S.config = std::async(std::launch::async, get, std::string(dect2::dsn::kConfigUrl), configPath());
    }
    if (S.config.valid() && S.config.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Fetch f = S.config.get();
        if (f.ok) S.names.load(f.text);
    }
    if (S.feed.valid() && S.feed.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Fetch f = S.feed.get();
        dect2::dsn::Report r;
        if (f.ok && dect2::dsn::parseReport(f.text, &S.names, r)) {
            S.report = std::move(r); S.haveReport = true; S.failures = 0; S.status.clear();
        } else {
            S.failures++;
            S.status = f.ok ? "the answer was not a DSN report" : "could not reach eyes.nasa.gov";
        }
        // back off after failures: 5 s, 10 s, 20 s ... at most 2 minutes
        S.nextPoll = now + std::min(120.0, dect2::dsn::kPollSec * std::pow(2.0, std::min(S.failures, 5)));
    }
    if (!S.feed.valid() && now >= S.nextPoll) {
        // a query string keeps caches in between from answering with an old copy
        const std::string url = std::string(dect2::dsn::kFeedUrl) + "?r=" + std::to_string((long long)std::time(nullptr) / 5);
        S.feed = std::async(std::launch::async, get, url, plat::cacheDir() + "/dsn_now.xml");
        S.nextPoll = now + dect2::dsn::kPollSec;
    }
}

std::string clockText(int64_t ms) {
    if (ms <= 0) return "-";
    const std::time_t t = (std::time_t)(ms / 1000);
    std::tm g = *std::gmtime(&t);
    char b[32];
    std::strftime(b, sizeof b, "%H:%M:%S", &g);
    return b;
}

bool busy(const dect2::dsn::Dish& d) {
    for (auto& s : d.signals) if (s.active) return true;
    return false;
}

void tuneTo(App& a, double hz) {
    a.freqMhz = hz / 1e6;
    if (a.engine.running()) { a.tune.centerHz = hz; a.engine.retune(a.tune); }
}

void signalCell(App& a, const dect2::dsn::Signal& s, int row) {
    namespace d = dect2::dsn;
    if (!s.active) { ImGui::TextDisabled("%s off", d::bandName(s.band)); return; }
    if (s.up) ImGui::Text("%s %s, %.3g kW", d::bandName(s.band), d::kindName(s.kind), s.power);
    else ImGui::Text("%s %s, %s, %.1f dBm", d::bandName(s.band), d::kindName(s.kind), d::rateText(s.rateBps).c_str(), s.power);
    if (ImGui::IsItemHovered()) {
        std::string tip;
        char b[160];
        if (s.freqHz > 0) { snprintf(b, sizeof b, "%.6f MHz\n", s.freqHz / 1e6); tip += b; }
        else tip += "frequency not published in the feed\n";
        for (auto& al : d::allocations(s.band, !s.up)) { snprintf(b, sizeof b, "%s %s: %.0f - %.0f MHz\n", s.up ? "uplink" : "downlink", al.use, al.loMhz, al.hiMhz); tip += b; }
        if (!s.up && s.band == 'S') tip += "S band downlinks are within reach of a HackRF, Pluto or LimeSDR with a dish and an LNA";
        else if (!s.up && s.band == 'X') tip += "X band needs a downconverter (an 8.4 GHz LNB) in front of the radio";
        else if (!s.up && s.band == 'K') tip += "Ka band needs a downconverter and a precise dish";
        if (s.powerFlagged) tip += "\n(the feed had this power 300 dB low, a flag; corrected)";
        ImGui::SetTooltip("%s", tip.c_str());
    }
    if (!s.up && s.freqHz > 0) {
        ImGui::SameLine();
        ImGui::PushID(row);
        if (ImGui::SmallButton(tr("Tune"))) tuneTo(a, s.freqHz);
        ImGui::PopID();
    }
}

} // namespace

void dsnWindow(App& a) {
    if (!a.dsnOpen) return;
    poll();
    namespace d = dect2::dsn;
    ImGui::SetNextWindowSize(ImVec2(980 * gUi, 560 * gUi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("Deep Space Network"), &a.dsnOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (S.haveReport) ImGui::TextDisabled("%s %s UTC", tr("Report of"), clockText(S.report.timestampMs).c_str());
    else ImGui::TextDisabled("%s", tr("Fetching the DSN Now report ..."));
    ImGui::SameLine();
    ImGui::TextDisabled("· %s", tr("eyes.nasa.gov, every 5 s, read only"));
    if (!S.status.empty()) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", S.status.c_str()); }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 170 * gUi);
    ImGui::Checkbox(tr("Busy dishes only"), &S.activeOnly);
    if (!S.names.loaded()) { ImGui::TextDisabled("%s", tr("(spacecraft names arrive with the configuration file)")); }
    int row = 0;
    for (const auto& st : S.report.stations) {
        int shown = 0;
        for (auto& dsh : st.dishes) if (!S.activeOnly || busy(dsh)) shown++;
        char head[160];
        const std::string local = clockText(st.utcMs + st.tzOffsetMs);
        snprintf(head, sizeof head, "%s  ·  %s %s  ·  %d / %zu %s###dsnst%s", st.friendly.c_str(), tr("local"), local.c_str(), shown, st.dishes.size(),
                 tr("dishes busy"), st.name.c_str());
        if (!ImGui::CollapsingHeader(head, ImGuiTreeNodeFlags_DefaultOpen)) continue;
        if (shown == 0) { ImGui::TextDisabled("   %s", tr("no dish is busy here now")); continue; }
        if (!ImGui::BeginTable(("##dsn" + st.name).c_str(), 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) continue;
        ImGui::TableSetupColumn(tr("Dish")); ImGui::TableSetupColumn(tr("Az / El")); ImGui::TableSetupColumn(tr("Spacecraft"));
        ImGui::TableSetupColumn(tr("Distance, light time")); ImGui::TableSetupColumn(tr("Downlink"));
        ImGui::TableSetupColumn(tr("Uplink"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& dsh : st.dishes) {
            if (S.activeOnly && !busy(dsh)) continue;
            const size_t lines = std::max<size_t>(1, dsh.targets.size());
            for (size_t k = 0; k < lines; k++) {
                const d::Target* t = k < dsh.targets.size() ? &dsh.targets[k] : nullptr;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (k == 0) {
                    ImGui::Text("%s%s%s", dsh.name.c_str(), dsh.type.empty() ? "" : " ", dsh.type.c_str());
                    if (ImGui::IsItemHovered()) {
                        char b[200];
                        snprintf(b, sizeof b, "%s%s%s%s", dsh.activity.c_str(), dsh.mspa ? "\nseveral spacecraft in one beam (MSPA)" : "",
                                 dsh.array ? "\npart of an array of dishes" : "", dsh.ddor ? "\ndelta-DOR navigation measurement" : "");
                        ImGui::SetTooltip("%s", b);
                    }
                }
                ImGui::TableNextColumn();
                if (k == 0) ImGui::Text("%.0f° / %.0f°", dsh.azDeg, dsh.elDeg);
                ImGui::TableNextColumn();
                if (t) {
                    ImGui::TextUnformatted(t->name.empty() ? t->call.c_str() : t->name.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, id %d", t->call.c_str(), t->id);
                }
                ImGui::TableNextColumn();
                if (t) {
                    const double km = std::isfinite(t->downlegKm) ? t->downlegKm : t->uplegKm;
                    if (std::isfinite(km)) ImGui::Text("%s, %s", d::distanceText(km).c_str(), d::lightTimeText(km / 299792.458).c_str());
                    else ImGui::TextDisabled("-");
                    if (ImGui::IsItemHovered() && std::isfinite(t->rtltS)) ImGui::SetTooltip("%s %s", tr("round-trip light time in the feed:"), d::lightTimeText(t->rtltS).c_str());
                }
                const std::string call = t ? t->call : "";
                ImGui::TableNextColumn();
                for (auto& s : dsh.signals) if (!s.up && (call.empty() || s.spacecraft == call)) { signalCell(a, s, row++); break; }
                ImGui::TableNextColumn();
                for (auto& s : dsh.signals) if (s.up && (call.empty() || s.spacecraft == call)) { signalCell(a, s, row++); break; }
            }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Data: NASA/JPL DSN Now. Frequencies are often not published; hover a signal for its band plan. S band downlinks can be heard with "
                                 "an SDR, a dish and an LNA; X and Ka band need a downconverter."));
    ImGui::PopTextWrapPos();
    ImGui::End();
}

void dsnDevOpen(App& a) { a.dsnOpen = true; }
