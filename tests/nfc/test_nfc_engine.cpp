// NFC through the engine (setStandard(150)): the synthetic source plays the test reader and card, the receiver reports through latestRx(),
// the mode's registrations agree (tuning table, mode row, screen mapping).
#include "dect2/engine/engine.h"
#include "dect2/nfc/nfc_rx.h"
#include "dect2/nfc/nfc_tel.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    const ModeTuning mt = nfcTuning();
    CHECK(mt.stdMode == 150 && modeTuningById("nfc") && modeTuningById("nfc")->stdMode == 150 && modeTuning(150), "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 13.56e6 + mt.tuneOffsetHz;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 150;
    t.synth.snrDb = 35;
    t.synth.modeOpt[0] = 0;
    FileOptions fo;
    e.setStandard(150);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 149, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 6.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 149) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const NfcTelemetry& d = rx.nfc;
    printf("  %d reports: %s\n", reports, nfcSummary(d).c_str());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.fieldOn, "no field");
    CHECK(d.cards.size() == 1 && d.cards[0].uid.size() == 4, "%zu cards", d.cards.size());
    CHECK(d.blocksOk >= 14 && d.blocksBad == 0, "%llu good, %llu bad", (unsigned long long)d.blocksOk, (unsigned long long)d.blocksBad);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.nfc().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    e.stop();

    // the registrations
    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{148, \"NFC / RFID sniffer\", 8,") != std::string::npos, "mode row for family 148 missing");
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 148: return &kNfcUi;") != std::string::npos, "family 148 not mapped");
    const std::string cm = readFile("CMakeLists.txt");
    CHECK(cm.find(" nfc ") != std::string::npos || cm.find(" nfc)") != std::string::npos, "nfc not in DECT2_MODES");
    const std::string ui = readFile("app/nfc/nfc_ui.cpp");
    for (const char* s : {"a.rx.standard == 149", "Privacy:", "Field threshold", "nfc-laboratory"}) CHECK(ui.find(s) != std::string::npos, "screen string missing: %s", s);
    return testSummary("nfc_engine");
}
