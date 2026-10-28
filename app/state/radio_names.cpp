#include "radio_names.h"
#include "../app.h"
#include "../platform/platform.h"
#include <atomic>
#include <map>
#include <mutex>
#include <tuple>
#include <ctime>
#include <filesystem>
#include "dect2/common/u8file.h"

namespace radionames {
namespace {
dect2::RadioLabels gLabels;
std::string gLoadErr;
std::atomic<unsigned> gGen{1};
}  // namespace

std::string path() { return plat::dataDir() + "/radios/names.json"; }
const std::string& loadError() { return gLoadErr; }
unsigned generation() { return gGen.load(); }

void load() {
    std::string err;
    gLoadErr.clear();
    if (!gLabels.load(path(), err)) {
        std::error_code ec;
        if (std::filesystem::exists(dect2::u8file(path()), ec)) {
            // a damaged file must not be replaced by the next save without a trace: keep it as names.json.bad-<time>, then start empty
            char t[32];
            const time_t now = time(nullptr);
            strftime(t, sizeof t, "%Y%m%d-%H%M%S", localtime(&now));
            const std::string bad = path() + ".bad-" + t;
            std::filesystem::rename(dect2::u8file(path()), dect2::u8file(bad), ec);
            gLoadErr = std::string(tr("Radio names file was unreadable")) + " (" + err + "); " +
                       (ec ? std::string(tr("could not keep a copy")) : std::string(tr("saved a copy as ")) + bad) + ". " + tr("Started with no names.");
            fprintf(stderr, "radio names: %s\n", gLoadErr.c_str());
        }   // a missing file is the normal first start
    }
    gGen++;
}

std::string labelFor(const std::string& key) { return key.empty() ? std::string() : gLabels.get(key); }

bool setName(const std::string& key, const std::string& name, std::string& err) {
    if (key.empty()) { err = "this source cannot be named"; return false; }
    gLabels.set(key, dect2::cleanRadioName(name));
    gGen++;
    return gLabels.save(path(), err);
}

std::vector<dect2::RadioRow> rowsFor(const std::vector<dect2::DeviceInfo>& devices) {
    bool moved = false;
    std::vector<dect2::RadioRow> out = dect2::identifySources(devices, gLabels, &moved);
    if (moved) {
        std::string err;
        if (!gLabels.save(path(), err)) fprintf(stderr, "radio names: %s\n", err.c_str());
        gGen++;
    }
    return out;
}

}  // namespace radionames

const dect2::RadioRow* radioRow(const App& a, int idx) {
    if (idx < 0 || idx >= (int)a.devices.size()) return nullptr;
    // the rows are cached per receiver; a changed name, a longer or shorter list or a different radio at an index rebuilds them
    bool stale = a.radioRows.size() != a.devices.size() || a.radioRowsGen != radionames::generation();
    for (size_t i = 0; !stale && i < a.devices.size(); i++)
        stale = a.radioRows[i].dev.name != a.devices[i].name || a.radioRows[i].dev.serial != a.devices[i].serial || a.radioRows[i].dev.usbPath != a.devices[i].usbPath;
    if (stale) {
        a.radioRows = radionames::rowsFor(a.devices);
        a.radioRowsGen = radionames::generation();
    }
    return &a.radioRows[(size_t)idx];
}

std::string radioShownName(const App& a, int idx) {
    const dect2::RadioRow* r = radioRow(a, idx);
    if (!r) return std::string();
    return r->key.empty() ? r->dev.name : dect2::radioDisplayName(*r);
}

std::string radioShownName(const App& a, const dect2::DeviceInfo& d) {
    for (int i = 0; i < (int)a.devices.size(); i++)
        if (&a.devices[(size_t)i] == &d) return radioShownName(a, i);
    return d.name;
}

std::string radioKeyAt(const App& a, int idx) {
    const dect2::RadioRow* r = radioRow(a, idx);
    return r ? r->key : std::string();
}

int findRadio(const App& a, const std::string& key, const std::string& name) {
    if (!key.empty())
        for (int i = 2; i < (int)a.devices.size(); i++) {
            const dect2::RadioRow* r = radioRow(a, i);
            if (r && r->key == key) return i;
        }
    if (!name.empty())
        for (int i = 2; i < (int)a.devices.size(); i++) if (a.devices[i].name == name) return i;
    return -1;
}

std::string sharedSerialWarning(const App& a) {
    // radios that look the same (kind, board, serial): the first group of two or more
    std::map<std::tuple<int, std::string, std::string>, std::pair<int, bool>> g;   // count, any without a USB port
    for (int i = 0; i < (int)a.devices.size(); i++) {
        const dect2::RadioRow* r = radioRow(a, i);
        if (!r || r->dupIndex == 0 || !r->sharedSerial) continue;
        auto& e = g[{(int)r->dev.kind, r->dev.board, r->dev.serial}];
        e.first++;
        e.second |= r->dev.usbPath.empty();
    }
    for (const auto& [k, e] : g) {
        if (e.first < 2) continue;
        const std::string& serial = std::get<2>(k);
        char b[512];
        snprintf(b, sizeof b, e.second ? tr("Two radios share serial %s: these two cannot be told apart until they have unique serials.")
                                       : tr("Two radios share serial %s: names follow the USB port. Give them unique serials (RTL-SDR) to keep names when they move ports."),
                 serial.empty() ? tr("(none)") : serial.c_str());
        return b;
    }
    return std::string();
}
