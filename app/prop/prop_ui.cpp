// Tools > Propagation: where a transmitter can be received. Place the transmitter on the map (or use your location), enter power, antenna, frequency,
// bandwidth, the SNR the receiver needs and the UTC time; the shared map (the ADS-B map's tiles) shows the predicted SNR as a heat map, the range ring
// where the SNR is just met, and a readout for any clicked point. The model is core/include/dect2/prop/prop.h (docs/propagation.md): no terrain.
#include "../app.h"
#include "../adsb/adsb_map.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/prop/prop.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
namespace pr = dect2::prop;

struct Inputs {
    double txLat = 54.687, txLon = 25.280;
    double power = 10; int powerUnit = 0;       // 0 W, 1 dBW, 2 dBm
    bool powerIsEirp = false;
    double txGainDbi = 2.15, feederDb = 0;
    double fMHz = 145.5, bandwidthHz = 12500, requiredSnr = 12;
    double rxGainDbi = 0, rxNfDb = 6;
    int env = (int)pr::Env::Residential, ground = (int)pr::Ground::Average;
    double txH = 10, rxH = 2, sfi = 120;
    int year = 2026, month = 1, day = 1, hour = 12, minute = 0;
    int resolution = 1;                         // 0 coarse 40, 1 medium 80, 2 fine 140
    bool useNow = true;
};

struct Job {
    pr::Params p;
    double radiusKm = 0;
    int n = 0;
};

struct Result {
    pr::Params p;
    pr::Range range;
    pr::Grid grid;
    bool valid = false;
};

struct State {
    bool loaded = false;
    Inputs in;
    adsbmap::View map;
    bool probe = false; double pLat = 0, pLon = 0;
    int clickSets = 0;                          // 0 transmitter, 1 probe point
    char coordBuf[96] = "";
    std::string msg;
    // worker
    std::mutex mu;
    std::thread th;
    std::atomic<bool> busy{false};
    Result result;                              // under mu
    Inputs lastIn; bool haveLast = false;
    ~State() { if (th.joinable()) th.join(); }   // never leave a joinable thread at exit
};
State S;

double eirpDbw(const Inputs& i) {
    double dbw = i.powerUnit == 0 ? 10 * std::log10(std::max(i.power, 1e-9)) : i.powerUnit == 1 ? i.power : i.power - 30.0;
    if (!i.powerIsEirp) dbw += i.txGainDbi - i.feederDb;
    return dbw;
}

pr::Params paramsOf(const Inputs& i) {
    pr::Params p;
    p.txLat = i.txLat; p.txLon = i.txLon; p.eirpDbw = eirpDbw(i);
    p.fMHz = std::max(i.fMHz, 0.01); p.bandwidthHz = std::max(i.bandwidthHz, 1.0);
    p.rxGainDbi = i.rxGainDbi; p.rxNfDb = i.rxNfDb;
    p.env = (pr::Env)i.env; p.ground = (pr::Ground)i.ground;
    p.txHeightM = std::max(i.txH, 0.0); p.rxHeightM = std::max(i.rxH, 0.0);
    p.sfi = i.sfi; p.year = i.year; p.month = i.month; p.day = i.day;
    p.utcHour = i.hour + i.minute / 60.0;
    p.requiredSnrDb = i.requiredSnr;
    return p;
}

bool sameIn(const Inputs& a, const Inputs& b) {   // field by field: no padding bytes in the comparison
    return a.txLat == b.txLat && a.txLon == b.txLon && a.power == b.power && a.powerUnit == b.powerUnit && a.powerIsEirp == b.powerIsEirp &&
           a.txGainDbi == b.txGainDbi && a.feederDb == b.feederDb && a.fMHz == b.fMHz && a.bandwidthHz == b.bandwidthHz &&
           a.requiredSnr == b.requiredSnr && a.rxGainDbi == b.rxGainDbi && a.rxNfDb == b.rxNfDb && a.env == b.env && a.ground == b.ground &&
           a.txH == b.txH && a.rxH == b.rxH && a.sfi == b.sfi && a.year == b.year && a.month == b.month && a.day == b.day &&
           a.hour == b.hour && a.minute == b.minute && a.resolution == b.resolution && a.useNow == b.useNow;
}

void nowUtc(Inputs& i) {
    const std::time_t t = std::time(nullptr);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    i.year = g.tm_year + 1900; i.month = g.tm_mon + 1; i.day = g.tm_mday; i.hour = g.tm_hour; i.minute = g.tm_min;
}

void load(App&) {
    if (S.loaded) return;
    S.loaded = true;
    auto& pf = plat::prefs();
    double rl, ro; receiverLocation(rl, ro);
    Inputs& i = S.in;
    i.txLat = pf.getD("prop_lat", rl); i.txLon = pf.getD("prop_lon", ro);
    i.power = pf.getD("prop_power", i.power); i.powerUnit = (int)pf.getI("prop_punit", i.powerUnit);
    i.powerIsEirp = pf.getB("prop_eirp", i.powerIsEirp);
    i.txGainDbi = pf.getD("prop_txg", i.txGainDbi); i.feederDb = pf.getD("prop_feed", i.feederDb);
    i.fMHz = pf.getD("prop_f", i.fMHz); i.bandwidthHz = pf.getD("prop_bw", i.bandwidthHz); i.requiredSnr = pf.getD("prop_snr", i.requiredSnr);
    i.rxGainDbi = pf.getD("prop_rxg", i.rxGainDbi); i.rxNfDb = pf.getD("prop_nf", i.rxNfDb);
    i.env = (int)pf.getI("prop_env", i.env); i.ground = (int)pf.getI("prop_gnd", i.ground);
    i.txH = pf.getD("prop_txh", i.txH); i.rxH = pf.getD("prop_rxh", i.rxH); i.sfi = pf.getD("prop_sfi", i.sfi);
    i.resolution = (int)pf.getI("prop_res", i.resolution); i.useNow = pf.getB("prop_now", true);
    nowUtc(i);
    if (!i.useNow) {
        i.year = (int)pf.getI("prop_y", i.year); i.month = (int)pf.getI("prop_mo", i.month); i.day = (int)pf.getI("prop_d", i.day);
        i.hour = (int)pf.getI("prop_h", i.hour); i.minute = (int)pf.getI("prop_mi", i.minute);
    }
    S.map.lat = i.txLat; S.map.lon = i.txLon; S.map.zoom = (int)pf.getI("prop_zoom", 8);
    S.pLat = i.txLat; S.pLon = i.txLon;
}

void save() {
    auto& pf = plat::prefs();
    const Inputs& i = S.in;
    pf.setD("prop_lat", i.txLat); pf.setD("prop_lon", i.txLon);
    pf.setD("prop_power", i.power); pf.setI("prop_punit", i.powerUnit); pf.setB("prop_eirp", i.powerIsEirp);
    pf.setD("prop_txg", i.txGainDbi); pf.setD("prop_feed", i.feederDb);
    pf.setD("prop_f", i.fMHz); pf.setD("prop_bw", i.bandwidthHz); pf.setD("prop_snr", i.requiredSnr);
    pf.setD("prop_rxg", i.rxGainDbi); pf.setD("prop_nf", i.rxNfDb);
    pf.setI("prop_env", i.env); pf.setI("prop_gnd", i.ground);
    pf.setD("prop_txh", i.txH); pf.setD("prop_rxh", i.rxH); pf.setD("prop_sfi", i.sfi);
    pf.setI("prop_res", i.resolution); pf.setB("prop_now", i.useNow);
    pf.setI("prop_y", i.year); pf.setI("prop_mo", i.month); pf.setI("prop_d", i.day); pf.setI("prop_h", i.hour); pf.setI("prop_mi", i.minute);
    pf.setI("prop_zoom", S.map.zoom);
}

// the grid off the UI thread: one job at a time, the next starts when the inputs changed meanwhile
void startJob() {
    if (S.th.joinable()) S.th.join();
    S.busy = true;
    Job j;
    j.p = paramsOf(S.in);
    j.n = S.in.resolution == 0 ? 40 : S.in.resolution == 2 ? 140 : 80;
    S.lastIn = S.in; S.haveLast = true;
    S.th = std::thread([j]() mutable {
        Result r;
        r.p = j.p;
        r.range = pr::findRange(j.p);
        double rad = r.range.any ? r.range.maxKm * 1.25 : 60.0;
        rad = std::min(std::max(rad, 5.0), 6000.0);
        r.grid = pr::computeGrid(j.p, rad, j.n);
        r.valid = true;
        { std::lock_guard<std::mutex> lk(S.mu); S.result = std::move(r); }
        S.busy = false;
    });
}

void schedule() {
    if (S.busy) return;
    if (S.th.joinable()) S.th.join();
    if (!S.haveLast || !sameIn(S.lastIn, S.in)) startJob();
}

// colour of a cell by its margin over the SNR the receiver needs
ImU32 marginColour(float m, int alpha) {
    struct Stop { float m; int r, g, b, a; };
    static const Stop st[] = {{-25, 40, 50, 120, 0}, {-12, 70, 80, 170, 70}, {0, 230, 90, 60, 150}, {6, 245, 200, 60, 170},
                              {15, 90, 210, 90, 175}, {30, 60, 200, 220, 185}, {45, 240, 240, 255, 200}};
    const int n = (int)(sizeof st / sizeof st[0]);
    if (m <= st[0].m) return IM_COL32(st[0].r, st[0].g, st[0].b, 0);
    if (m >= st[n - 1].m) return IM_COL32(st[n - 1].r, st[n - 1].g, st[n - 1].b, st[n - 1].a * alpha / 255);
    for (int i = 1; i < n; i++)
        if (m <= st[i].m) {
            const float t = (m - st[i - 1].m) / (st[i].m - st[i - 1].m);
            auto L = [t](int a, int b) { return (int)(a + (b - a) * t); };
            return IM_COL32(L(st[i - 1].r, st[i].r), L(st[i - 1].g, st[i].g), L(st[i - 1].b, st[i].b), L(st[i - 1].a, st[i].a) * alpha / 255);
        }
    return 0;
}

void ring(ImDrawList* dl, double lat, double lon, double km, ImU32 col) {
    if (km <= 0) return;
    ImVec2 pts[97];
    for (int i = 0; i <= 96; i++) {
        double la, lo; pr::destination(lat, lon, i * 360.0 / 96, km, la, lo);
        pts[i] = adsbmap::project(la, lo);
    }
    dl->AddPolyline(pts, 97, col, ImDrawFlags_None, 2.0f * gUi);
}

void drawHeat(ImDrawList* dl, const Result& r) {
    const pr::Grid& g = r.grid;
    if (g.n < 2 || g.snr.empty()) return;
    for (int y = 0; y < g.n; y++) {
        const double la0 = g.lat0 + (g.lat1 - g.lat0) * y / g.n, la1 = g.lat0 + (g.lat1 - g.lat0) * (y + 1) / g.n;
        for (int x = 0; x < g.n; x++) {
            const float m = g.snr[(size_t)y * g.n + x] - (float)r.p.requiredSnrDb;
            const ImU32 c = marginColour(m, 255);
            if ((c >> 24) == 0) continue;
            const double lo0 = g.lon0 + (g.lon1 - g.lon0) * x / g.n, lo1 = g.lon0 + (g.lon1 - g.lon0) * (x + 1) / g.n;
            const ImVec2 a = adsbmap::project(la0, lo0), b = adsbmap::project(la1, lo1);
            dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x) + 1, std::max(a.y, b.y) + 1), c);
        }
    }
}

void legend(float x, float y, const pr::Params& p) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = 190 * gUi, h = 12 * gUi;
    dl->AddRectFilled(ImVec2(x - 6 * gUi, y - 6 * gUi), ImVec2(x + w + 6 * gUi, y + h + 34 * gUi), IM_COL32(15, 18, 22, 200), 4 * gUi);
    const float lo = -25, hi = 45;
    for (int i = 0; i < 70; i++) {
        const float m = lo + (hi - lo) * i / 69.0f;
        dl->AddRectFilled(ImVec2(x + w * i / 70.0f, y), ImVec2(x + w * (i + 1) / 70.0f + 1, y + h), marginColour(m, 255));
    }
    const float zx = x + w * (0 - lo) / (hi - lo);
    dl->AddLine(ImVec2(zx, y - 3 * gUi), ImVec2(zx, y + h + 3 * gUi), IM_COL32(255, 255, 255, 230), 1.5f * gUi);
    char b[96];
    snprintf(b, sizeof b, "SNR %.0f dB", p.requiredSnrDb + lo);
    dl->AddText(ImVec2(x, y + h + 3 * gUi), IM_COL32(210, 215, 220, 255), b);
    snprintf(b, sizeof b, "%.0f dB", p.requiredSnrDb + hi);
    dl->AddText(ImVec2(x + w - ImGui::CalcTextSize(b).x, y + h + 3 * gUi), IM_COL32(210, 215, 220, 255), b);
    snprintf(b, sizeof b, "needed: %.0f dB (white line); white ring: range", p.requiredSnrDb);
    dl->AddText(ImVec2(x, y + h + 3 * gUi + ImGui::GetTextLineHeight()), IM_COL32(190, 196, 202, 255), b);
}

bool dInput(const char* label, double* v, float w, const char* fmt = "%.4g") {
    ImGui::SetNextItemWidth(w);
    return ImGui::InputDouble(label, v, 0, 0, fmt);
}

void inputsUi(App& a) {
    Inputs& i = S.in;
    const float w = 80 * gUi;
    // transmitter place
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Transmitter");
    ImGui::SameLine();
    ImGui::Text("%.5f, %.5f  %s", i.txLat, i.txLon, dect2::toMaidenhead(i.txLat, i.txLon, 6).c_str());
    ImGui::SameLine(0, 10 * gUi);
    if (ImGui::SmallButton("Use my location")) { receiverLocation(i.txLat, i.txLon); S.map.lat = i.txLat; S.map.lon = i.txLon; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::InputTextWithHint("##propcoord", "type coordinates, Enter", S.coordBuf, sizeof S.coordBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        double la, lo;
        if (dect2::parseCoordinates(S.coordBuf, la, lo)) { i.txLat = la; i.txLon = lo; S.map.lat = la; S.map.lon = lo; S.msg.clear(); }
        else S.msg = "not a position";
    }
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("a map click sets");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    ImGui::Combo("##propclick", &S.clickSets, "the transmitter\0a probe point\0");
    if (!S.msg.empty()) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", S.msg.c_str()); }

    // power
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Power");
    ImGui::SameLine(); dInput("##proppow", &i.power, w);
    ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ImGui::Combo("##propunit", &i.powerUnit, "W\0dBW\0dBm\0");
    ImGui::SameLine(0, 8 * gUi); ImGui::Checkbox("this is the EIRP##propeirp", &i.powerIsEirp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Tick when the number is already the radiated power including the antenna gain (EIRP). Otherwise the antenna gain below is added.");
    if (!i.powerIsEirp) {
        ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("antenna gain"); ImGui::SameLine(); dInput("##proptxg", &i.txGainDbi, 60 * gUi); ImGui::SameLine(); ImGui::TextDisabled("dBi");
        ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("feeder loss"); ImGui::SameLine(); dInput("##propfeed", &i.feederDb, 50 * gUi); ImGui::SameLine(); ImGui::TextDisabled("dB");
    }
    ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("= EIRP %.1f dBW (%.1f dBm)", eirpDbw(i), eirpDbw(i) + 30);

    // signal
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Frequency");
    ImGui::SameLine(); dInput("##propf", &i.fMHz, w); ImGui::SameLine(); ImGui::TextDisabled("MHz");
    ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("bandwidth"); ImGui::SameLine(); dInput("##propbw", &i.bandwidthHz, w); ImGui::SameLine(); ImGui::TextDisabled("Hz");
    ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("SNR needed"); ImGui::SameLine(); dInput("##propsnr", &i.requiredSnr, 55 * gUi); ImGui::SameLine(); ImGui::TextDisabled("dB");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The signal to noise ratio (in the bandwidth above) at which your receiver or mode just works, e.g. 12 dB for SINAD on narrow FM, about -20 dB for FT8 in 2.5 kHz.");

    // receiver
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Receiver");
    ImGui::SameLine(); ImGui::TextDisabled("antenna gain"); ImGui::SameLine(); dInput("##proprxg", &i.rxGainDbi, 55 * gUi); ImGui::SameLine(); ImGui::TextDisabled("dBi");
    ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("noise figure"); ImGui::SameLine(); dInput("##propnf", &i.rxNfDb, 50 * gUi); ImGui::SameLine(); ImGui::TextDisabled("dB");
    ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("noise at the receiver"); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    ImGui::Combo("##propenv", &i.env, "quiet rural\0rural\0residential\0city\0");
    if (i.fMHz >= 30) {
        ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("heights TX/RX"); ImGui::SameLine(); dInput("##proptxh", &i.txH, 45 * gUi); ImGui::SameLine(); dInput("##proprxh", &i.rxH, 45 * gUi); ImGui::SameLine(); ImGui::TextDisabled("m");
    } else {
        ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("ground"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::Combo("##propgnd", &i.ground, "sea\0wet land\0average land\0dry land\0");
        ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("solar flux (SFI)"); ImGui::SameLine(); dInput("##propsfi", &i.sfi, 55 * gUi);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The 10.7 cm solar flux index, 65 (quiet) to 250 (very active). Enter today's value from any space-weather source; nothing is downloaded here.");
    }

    // time
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Date UTC");
    ImGui::SameLine();
    if (ImGui::Checkbox("now##propnow", &i.useNow)) {}
    if (i.useNow) { nowUtc(i); ImGui::SameLine(); ImGui::TextDisabled("%04d-%02d-%02d %02d:%02d", i.year, i.month, i.day, i.hour, i.minute); }
    else {
        ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ImGui::InputInt("##propy", &i.year, 0, 0);
        ImGui::SameLine(); ImGui::SetNextItemWidth(36 * gUi); ImGui::InputInt("##propmo", &i.month, 0, 0);
        ImGui::SameLine(); ImGui::SetNextItemWidth(36 * gUi); ImGui::InputInt("##propd", &i.day, 0, 0);
        ImGui::SameLine(); ImGui::SetNextItemWidth(36 * gUi); ImGui::InputInt("##proph", &i.hour, 0, 0);
        ImGui::SameLine(); ImGui::SetNextItemWidth(36 * gUi); ImGui::InputInt("##propmi", &i.minute, 0, 0);
        ImGui::SameLine(); ImGui::TextDisabled("year month day hour min");
        i.year = std::clamp(i.year, 1990, 2100); i.month = std::clamp(i.month, 1, 12); i.day = std::clamp(i.day, 1, 31);
        i.hour = std::clamp(i.hour, 0, 23); i.minute = std::clamp(i.minute, 0, 59);
    }
    ImGui::SameLine(0, 12 * gUi); ImGui::TextDisabled("resolution"); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    ImGui::Combo("##propres", &i.resolution, "coarse (40)\0medium (80)\0fine (140)\0");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Grid cells per side. Coarse is the cheapest, for a phone or a Raspberry Pi.");
    (void)a;
    i.fMHz = std::clamp(i.fMHz, 0.01, 100000.0);
    i.bandwidthHz = std::max(i.bandwidthHz, 1.0);
}

void readout(const Result& r) {
    const pr::Params& p = r.p;
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "%s", pr::modelName(p));
    ImGui::SameLine(); ImGui::TextDisabled("(no terrain: hills, buildings and trees are not modelled)");
    ImGui::TextWrapped("%s", pr::modelLimits(p).c_str());
    if (r.range.any) {
        if (r.range.minKm > 1) ImGui::Text("SNR %.0f dB is met from %.0f km out to %.0f km (nothing closer: skip zone).", p.requiredSnrDb, r.range.minKm, r.range.maxKm);
        else ImGui::Text("SNR %.0f dB is met out to %.0f km.", p.requiredSnrDb, r.range.maxKm);
        if (p.fMHz >= 30) ImGui::SameLine(), ImGui::TextDisabled("(radio horizon %.0f km)", pr::radioHorizonKm(p.txHeightM, p.rxHeightM));
    } else ImGui::TextColored(pal::warnAmber(), "The required SNR of %.0f dB is not met anywhere with these settings.", p.requiredSnrDb);
    if (S.probe) {
        const pr::Point q = pr::evaluateAt(p, S.pLat, S.pLon);
        ImGui::Separator();
        ImGui::Text("Point %.4f, %.4f  %s", S.pLat, S.pLon, dect2::toMaidenhead(S.pLat, S.pLon, 6).c_str());
        ImGui::Text("distance %.1f km   model: %s   path loss %.1f dB", q.distKm, pr::pointModelName(q.model), q.pathLossDb);
        ImGui::Text("received %.1f dBm   field %.1f dBuV/m   noise %.1f dBm (%.0f Hz)", q.rxDbm, q.fieldDbuvm, q.noiseDbm, p.bandwidthHz);
        const ImVec4 c = q.marginDb >= 0 ? ImVec4(0.5f, 0.9f, 0.5f, 1) : ImVec4(0.95f, 0.5f, 0.45f, 1);
        ImGui::TextColored(c, "SNR %.1f dB   margin %+.1f dB over the %.0f dB needed", q.snrDb, q.marginDb, p.requiredSnrDb);
        if (p.fMHz < 30) {
            if (q.hops > 0) ImGui::Text("skywave: %d hop(s), MUF about %.1f MHz, D-layer absorption %.1f dB", q.hops, q.muf, q.absorptionDb);
            else ImGui::Text("no skywave: %.1f MHz is above the MUF of about %.1f MHz for this distance", p.fMHz, q.muf);
        }
    } else ImGui::TextDisabled("Choose \"a probe point\" in the box above and click the map for the numbers at one place.");
}

void mapView(App& a) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(180 * gUi, ImGui::GetContentRegionAvail().y - 150 * gUi));
    adsbmap::draw(S.map, size);
    ImVec2 click;
    if (adsbmap::clickedAt(click)) {
        double la, lo; adsbmap::unproject(click, la, lo); lo = std::fmod(lo + 540, 360) - 180;
        if (S.clickSets == 0) { S.in.txLat = la; S.in.txLon = lo; } else { S.probe = true; S.pLat = la; S.pLon = lo; }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    Result res; bool have = false;
    { std::lock_guard<std::mutex> lk(S.mu); if (S.result.valid) { res = S.result; have = true; } }
    if (have) {
        drawHeat(dl, res);
        // the ring is drawn around the transmitter the result was computed for
        if (res.range.any) {
            ring(dl, res.p.txLat, res.p.txLon, res.range.maxKm, IM_COL32(255, 255, 255, 230));
            if (res.range.minKm > 1) ring(dl, res.p.txLat, res.p.txLon, res.range.minKm, IM_COL32(255, 255, 255, 140));
        }
    }
    const ImVec2 q = adsbmap::project(S.in.txLat, S.in.txLon);
    const ImU32 c = IM_COL32(240, 90, 80, 255);
    dl->AddTriangleFilled(ImVec2(q.x - 4 * gUi, q.y - 10 * gUi), ImVec2(q.x + 4 * gUi, q.y - 10 * gUi), q, c);
    dl->AddCircleFilled(ImVec2(q.x, q.y - 13 * gUi), 6 * gUi, c);
    dl->AddCircleFilled(ImVec2(q.x, q.y - 13 * gUi), 2.2f * gUi, IM_COL32(255, 255, 255, 230));
    if (S.probe) {
        const ImVec2 pp = adsbmap::project(S.pLat, S.pLon);
        dl->AddCircle(pp, 6 * gUi, IM_COL32(255, 255, 255, 255), 16, 2 * gUi);
        dl->AddCircleFilled(pp, 2 * gUi, IM_COL32(255, 255, 255, 255));
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+##propz", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-##propz", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("TX##propc")) { S.map.lat = S.in.txLat; S.map.lon = S.in.txLon; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Centre the map on the transmitter");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the transmitter or your location");
    if (S.busy) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "computing..."); }
    if (have) legend(p0.x + size.x - 206 * gUi, p0.y + size.y - 62 * gUi, res.p);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));
    (void)a;
}

} // namespace

void propagationWindow(App& a) {
    if (!a.propOpen) {
        if (S.th.joinable() && !S.busy) S.th.join();
        return;
    }
    load(a);
    ImGui::SetNextWindowSize(ImVec2(920 * gUi, 760 * gUi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Propagation", &a.propOpen)) { ImGui::End(); return; }
    inputsUi(a);
    schedule();
    mapView(a);
    Result r; bool have = false;
    { std::lock_guard<std::mutex> lk(S.mu); if (S.result.valid) { r = S.result; have = true; } }
    if (have) readout(r); else ImGui::TextDisabled("computing...");
    static Inputs saved; static bool savedOnce = false;
    if (!savedOnce || !sameIn(saved, S.in)) { save(); saved = S.in; savedOnce = true; }
    ImGui::End();
}
