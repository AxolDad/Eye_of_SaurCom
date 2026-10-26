// Deep Space Network Now (core/src/net/dsn.cpp over the Rust crate zorcom_dsn): the recorded report and configuration of 2026-10-23
// turned into stations, dishes, targets and signals with names, and the text helpers.
#include "dect2/net/dsn.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace dect2::dsn;
#include "../common/test_util.h"

static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

int main() {
    const std::string dir = ZORCOM_DSN_TESTDATA;
    const std::string feed = readFile(dir + "/dsn_sample.xml"), config = readFile(dir + "/config_sample.xml");
    CHECK(!feed.empty() && !config.empty(), "test data in %s", dir.c_str());
    Names names;
    CHECK(names.load(config) && names.loaded() && names.spacecraftCount() >= 150, "configuration");
    CHECK(names.spacecraft("NHPC").find("New Horizons") != std::string::npos, "NHPC is %s", names.spacecraft("NHPC").c_str());
    CHECK(names.dishType("DSS43") == "70M" && names.dishType("DSS99").empty(), "dish types");
    CHECK(!names.load("<html/>") && names.loaded(), "a bad configuration keeps the old one");
    Report r;
    CHECK(parseReport(feed, &names, r), "report");
    CHECK(r.stations.size() == 3 && r.stations[0].friendly == "Goldstone" && r.stations[2].friendly == "Canberra", "stations");
    CHECK(std::fabs(r.stations[0].lat - 35.2443523) < 1e-9, "Goldstone latitude from the configuration %f", r.stations[0].lat);
    size_t dishes = 0, signals = 0;
    for (auto& s : r.stations) for (auto& d : s.dishes) { dishes++; signals += d.signals.size(); }
    CHECK(dishes == 14 && signals == 28, "dishes %zu signals %zu", dishes, signals);
    const Dish* d14 = nullptr;
    for (auto& d : r.stations[0].dishes) if (d.name == "DSS14") d14 = &d;
    CHECK(d14 && d14->type == "70M" && d14->activity == "Engineering Upgrades" && std::isnan(d14->windKmh), "DSS14");
    bool sawNh = false;
    for (auto& s : r.stations) for (auto& d : s.dishes) for (auto& t : d.targets)
        if (t.call == "NHPC") {
            sawNh = true;
            CHECK(t.name.find("New Horizons") != std::string::npos, "target named %s", t.name.c_str());
            for (auto& g : d.signals) if (!g.up && g.spacecraft == "NHPC") CHECK(g.active && g.kind == 2 && g.band == 'X' && g.rateBps == 49.99 && g.power == -160, "NHPC downlink");
        }
    CHECK(sawNh, "New Horizons is in the report");
    Report none;
    CHECK(!parseReport("<html><body>DSN Now</body></html>", &names, none) && none.stations.empty(), "the web page is not a report");
    CHECK(parseReport(feed, nullptr, none) && none.stations[0].dishes[0].type.empty(), "without names");
    // helpers
    CHECK(distanceText(384400) == "384400 km", "%s", distanceText(384400).c_str());
    CHECK(distanceText(91.4e6) == "91.40 million km (0.611 AU)", "%s", distanceText(91.4e6).c_str());
    CHECK(distanceText(25.3e9) == "25.30 billion km (169.1 AU)", "%s", distanceText(25.3e9).c_str());
    CHECK(distanceText(NAN) == "-", "unknown distance");
    CHECK(lightTimeText(1.28) == "1.3 s" && lightTimeText(600) == "10.0 min" && lightTimeText(84400) == "23.44 h", "light times");
    CHECK(rateText(49.99) == "49.99 bit/s" && rateText(146400) == "146.4 kbit/s" && rateText(0) == "-", "rates");
    CHECK(allocations('S', true).size() == 2 && allocations('S', true)[1].loMhz == 2290 && allocations('K', false).size() == 1, "allocations");
    CHECK(std::string(bandName('K')) == "Ka" && std::string(kindName(1)) == "carrier", "names");
    printf(fails ? "FAILED\n" : "dsn ok\n");
    return fails ? 1 : 0;
}
