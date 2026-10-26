// The Messages window (View > Messages): the unified database of decoded messages (core/src/msgdb) with search, filters, a timeline, export and the
// retention settings. The database is fed by a tap on the automation event bus; voice and encrypted content never reaches it.
#include "../app.h"
#include "dect2/common/u8file.h"
#include "dect2/automation/engine.h"
#include "dect2/msgdb/msgdb.h"
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
namespace mdb = dect2::msgdb;
namespace au = dect2::automation;
namespace fs = std::filesystem;

struct St {
    bool started = false;
    bool record = true;
    int maxRowsK = 1000;     // thousand rows
    int maxMB = 512;
    int keepDays = 0;
    char text[160] = "";
    char ident[64] = "";
    int modeSel = 0, range = 0;
    bool posOnly = false;
    int page = 0;
    std::vector<mdb::Record> rows;
    std::vector<std::string> modes;
    int64_t matches = 0;
    mdb::Stats stats;
    double lastQ = 0;
    bool dirty = true;
    int64_t selId = -1;       // the picked message by its id: the list refreshes every 3 s and new messages move the rows
    mdb::Record selRec;       // a copy, so the detail stays when the message leaves the page
    bool clearAsk = false;
    std::string note;
    std::string err;
};
St S;
constexpr int kPage = 300;

std::string dir() { return plat::dataDir() + "/messages"; }

void saveSettings() {
    std::error_code ec;
    fs::create_directories(fs::path(reinterpret_cast<const char8_t*>(dir().c_str())), ec);
    std::ofstream o(dect2::u8file(dir() + "/settings.txt"));
    o << "record=" << S.record << "\nmax_rows_k=" << S.maxRowsK << "\nmax_mb=" << S.maxMB << "\nkeep_days=" << S.keepDays << "\n";
}

void loadSettings() {
    std::ifstream f(dect2::u8file(dir() + "/settings.txt"));
    std::string l;
    while (std::getline(f, l)) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq);
        const int v = atoi(l.c_str() + eq + 1);
        if (k == "record") S.record = v != 0;
        else if (k == "max_rows_k") S.maxRowsK = std::max(0, std::min(v, 100000));
        else if (k == "max_mb") S.maxMB = std::max(0, std::min(v, 100000));
        else if (k == "keep_days") S.keepDays = std::max(0, std::min(v, 36500));
    }
}

void applySettings() {
    mdb::Limits l;
    l.maxRows = (int64_t)S.maxRowsK * 1000;
    l.maxBytes = (int64_t)S.maxMB << 20;
    l.maxAgeDays = S.keepDays;
    mdb::global().setLimits(l);
    const uint32_t mask = (1u << au::FAdsb) | (1u << au::FAcars) | (1u << au::FVdl2) | (1u << au::FHfdl) | (1u << au::FPager) | (1u << au::FAis) | (1u << au::FAprs) |
                          (1u << au::FSonde) | (1u << au::FLora) | (1u << au::FBle) | (1u << au::FRid) | (1u << au::FDsc);
    if (S.record) au::global().setTap([](const au::Event& e) { mdb::global().enqueue(e); }, mask);
    else au::global().setTap(nullptr, 0);
}

void start() {
    S.started = true;
    loadSettings();
    std::error_code ec;
    fs::create_directories(fs::path(reinterpret_cast<const char8_t*>(dir().c_str())), ec);
    if (!mdb::global().open(dir() + "/messages.db", &S.err)) return;
    applySettings();
    mdb::global().prune();
    mdb::global().start();
}

std::string when(double t) {
    time_t s = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &s);
#else
    gmtime_r(&s, &g);
#endif
    char b[40];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &g);
    return b;
}

mdb::Filter filter() {
    mdb::Filter f;
    f.text = S.text;
    f.ident = S.ident;
    if (S.modeSel > 0 && S.modeSel <= (int)S.modes.size()) f.mode = S.modes[(size_t)S.modeSel - 1];
    static const double span[] = {0, 3600, 86400, 7 * 86400, 30 * 86400};
    if (S.range > 0) f.since = au::wallNow() - span[S.range];
    f.posOnly = S.posOnly;
    f.limit = kPage;
    f.offset = S.page * kPage;
    return f;
}

void refresh() {
    mdb::Db& db = mdb::global();
    const mdb::Filter f = filter();
    S.rows = db.query(f);
    S.matches = db.count(f);
    S.stats = db.stats();
    S.modes = db.modes();
    if (S.modeSel > (int)S.modes.size()) S.modeSel = 0;
    S.dirty = false;
}

void exportButton(const char* label, const char* fmt, const char* ext) {
    if (!ImGui::Button(label)) return;
    const std::string nm = std::string("messages-") + std::to_string((long long)au::wallNow()) + ext;
    const std::string path = saveFileDialog(nm.c_str());
    if (path.empty()) return;
    mdb::Filter f = filter();
    f.oldestFirst = true;
    std::ofstream o(dect2::u8file(path), std::ios::binary);
    o << mdb::global().exportAs(f, fmt);
    o.flush();
    S.note = o ? std::string(tr("saved")) + " " + path : std::string(tr("could not write")) + " " + path;
}

void windowPanel(App& a) {
    ImGui::SetNextWindowSize(ImVec2(980, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Messages"), &a.msgdbOpen)) { ImGui::End(); return; }
    mdb::Db& db = mdb::global();
    if (!db.isOpen()) {
        ImGui::TextWrapped("%s %s", tr("The message database could not be opened:"), S.err.c_str());
        ImGui::End();
        return;
    }
    bool ch = false;
    ImGui::SetNextItemWidth(260);
    ch |= ImGui::InputTextWithHint("##q", tr("Search text (words, prefix*)"), S.text, sizeof S.text);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    ch |= ImGui::InputTextWithHint("##id", tr("Identity"), S.ident, sizeof S.ident);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::BeginCombo("##mode", S.modeSel > 0 && S.modeSel <= (int)S.modes.size() ? S.modes[(size_t)S.modeSel - 1].c_str() : tr("All modes"))) {
        if (ImGui::Selectable(tr("All modes"), S.modeSel == 0)) { S.modeSel = 0; ch = true; }
        for (size_t i = 0; i < S.modes.size(); i++) if (ImGui::Selectable(S.modes[i].c_str(), S.modeSel == (int)i + 1)) { S.modeSel = (int)i + 1; ch = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    ch |= ImGui::Combo("##range", &S.range, "All time\0Last hour\0Last 24 hours\0Last 7 days\0Last 30 days\0");
    ImGui::SameLine();
    ch |= ImGui::Checkbox(tr("With position"), &S.posOnly);
    if (ch) { S.page = 0; S.dirty = true; }
    if (S.dirty || ImGui::GetTime() - S.lastQ > 3.0) { S.lastQ = ImGui::GetTime(); refresh(); }

    ImGui::TextDisabled("%lld %s, %lld %s, %.1f MB", (long long)S.matches, tr("matching"), (long long)S.stats.rows, tr("stored"), (double)S.stats.bytes / 1048576.0);
    ImGui::SameLine();
    if (S.page > 0 && ImGui::SmallButton(tr("Newer"))) { S.page--; S.dirty = true; }
    ImGui::SameLine();
    if ((S.page + 1) * kPage < S.matches && ImGui::SmallButton(tr("Older"))) { S.page++; S.dirty = true; }

    const float h = ImGui::GetContentRegionAvail().y - 112;
    if (ImGui::BeginTable("msgs", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(120.f, h)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time (UTC)"), ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn(tr("Mode"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(tr("Identity"), ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn(tr("MHz"), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(tr("Message"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < S.rows.size(); i++) {
            const mdb::Record& r = S.rows[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char id[32];
            snprintf(id, sizeof id, "%s##%lld", when(r.time).c_str(), (long long)r.id);
            if (ImGui::Selectable(id, S.selId == r.id, ImGuiSelectableFlags_SpanAllColumns)) {
                if (S.selId == r.id) S.selId = -1; else { S.selId = r.id; S.selRec = r; }
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.mode.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.ident.c_str());
            ImGui::TableNextColumn(); if (r.freqHz > 0) ImGui::Text("%.4f", r.freqHz / 1e6);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.text.c_str());
        }
        ImGui::EndTable();
    }
    if (S.selId >= 0) {
        const mdb::Record& r = S.selRec;
        ImGui::BeginChild("detail", ImVec2(0, 60 * gUi), true);
        std::string e = r.extra;
        for (char& c : e) if (c == '\n') c = ' ';
        if (r.hasPos) {
            if (ImGui::SmallButton(tr("Show on map"))) mapShow(a, r.lat, r.lon);
            ImGui::SameLine();
        }
        ImGui::TextWrapped("%s  %s  %s", r.type.c_str(), r.hasPos ? (std::to_string(r.lat).substr(0, 8) + ", " + std::to_string(r.lon).substr(0, 8)).c_str() : "", e.c_str());
        ImGui::EndChild();
    }
    exportButton(tr("Export CSV"), "csv", ".csv");
    ImGui::SameLine();
    exportButton(tr("Export JSON"), "json", ".json");
    ImGui::SameLine();
    exportButton(tr("Export ADIF"), "adif", ".adi");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", S.note.c_str());
    if (ImGui::CollapsingHeader(tr("Storage"))) {
        bool sc = false;
        sc |= ImGui::Checkbox(tr("Record decoded messages"), &S.record);
        ImGui::SetNextItemWidth(120); sc |= ImGui::InputInt(tr("Most rows (thousands, 0 = no limit)"), &S.maxRowsK, 0, 0);
        ImGui::SetNextItemWidth(120); sc |= ImGui::InputInt(tr("Size cap in MB (0 = none)"), &S.maxMB, 0, 0);
        ImGui::SetNextItemWidth(120); sc |= ImGui::InputInt(tr("Keep days (0 = forever)"), &S.keepDays, 0, 0);
        if (sc) {
            S.maxRowsK = std::max(0, S.maxRowsK); S.maxMB = std::max(0, S.maxMB); S.keepDays = std::max(0, S.keepDays);
            applySettings();
            saveSettings();
        }
        if (ImGui::Button(tr("Apply limits now"))) { db.prune(); S.dirty = true; }
        ImGui::SameLine();
        if (!S.clearAsk) { if (ImGui::Button(tr("Clear all..."))) S.clearAsk = true; }
        else {
            if (ImGui::Button(tr("Really clear"))) { db.clear(); S.clearAsk = false; S.dirty = true; }
            ImGui::SameLine();
            if (ImGui::Button(tr("Keep"))) S.clearAsk = false;
        }
        ImGui::TextWrapped("%s %s/messages.db", tr("The database file:"), dir().c_str());
        ImGui::TextWrapped("%s", tr("Voice audio and encrypted traffic are never stored."));
    }
    ImGui::End();
}
}  // namespace

void msgdbFind(App& a, const std::string& ident) {
    snprintf(S.ident, sizeof S.ident, "%s", ident.c_str());
    S.text[0] = 0;
    S.modeSel = 0; S.range = 0; S.posOnly = false;
    S.page = 0; S.dirty = true;
    a.msgdbOpen = true;
    if (App* f = receiverById(1)) f->msgdbOpen = true;
}

void msgdbWindow(App& a) {
    if (a.rxId != 1) {   // one database for the program: RX 1's window
        if (App* f = receiverById(1)) { f->msgdbOpen = a.msgdbOpen; msgdbWindow(*f); a.msgdbOpen = f->msgdbOpen; }
        return;
    }
    if (!S.started) start();
    if (a.msgdbOpen) windowPanel(a);
}
