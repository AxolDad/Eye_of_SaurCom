// Cross-site fixes: df.bearing events from two or more stations close in frequency and time become one position fix.
#include "dect2/automation/event.h"
#include "dect2/df/position.h"
#include "dect2/df/xfix.h"
#include "../common/test_util.h"
#include <cmath>

using namespace dect2::df;
namespace au = dect2::automation;

namespace {
constexpr double kT0 = 1792232100.0;

au::Event bearing(const char* site, double t, double hz, double lat, double lon, double brg, double sigma = 3) {
    au::Event e("df.bearing");
    e.at(t, "DF", hz);
    e.num("bearing_deg", brg).num("sigma_deg", sigma).num("lat", lat).num("lon", lon);
    if (*site) e.str("site", site);
    return e;
}
double bearingTo(double la, double lo, double tla, double tlo) { double d, az; geodInverse(la, lo, tla, tlo, d, az); return az; }
}  // namespace

int main() {
    const double tla = 54.70, tlo = 25.30;
    double a_la, a_lo, b_la, b_lo;
    geodDirect(tla, tlo, 200, 7000, a_la, a_lo);   // two stations about 10 km apart, both with a view of the point
    geodDirect(tla, tlo, 100, 7000, b_la, b_lo);
    const double ba = bearingTo(a_la, a_lo, tla, tlo), bb = bearingTo(b_la, b_lo, tla, tlo);

    {
        CrossFixer cf;
        cf.add(bearing("", kT0, 433.92e6, a_la, a_lo, ba));
        cf.add(bearing("b", kT0 + 2, 433.93e6, b_la, b_lo, bb));
        auto fx = cf.fixes(kT0 + 3);
        CHECK(fx.size() == 1, "one fix: %zu", fx.size());
        if (fx.size() == 1) {
            double d, az;
            geodInverse(fx[0].fix.lat, fx[0].fix.lon, tla, tlo, d, az);
            CHECK(d < 50, "within 50 m: %.1f", d);
            CHECK(fx[0].sites.size() == 2 && fx[0].sites[0] == "" && fx[0].sites[1] == "b", "sites");
            CHECK(std::abs(fx[0].freqHz - 433.925e6) < 20e3, "frequency");
        }
    }
    {   // 30 kHz apart: not the same signal
        CrossFixer cf;
        cf.add(bearing("", kT0, 433.90e6, a_la, a_lo, ba));
        cf.add(bearing("b", kT0, 433.93e6, b_la, b_lo, bb));
        CHECK(cf.fixes(kT0 + 1).empty(), "30 kHz apart: no fix");
    }
    {   // one site only
        CrossFixer cf;
        cf.add(bearing("", kT0, 433.92e6, a_la, a_lo, ba));
        cf.add(bearing("", kT0 + 1, 433.92e6, a_la, a_lo, ba + 1));
        CHECK(cf.fixes(kT0 + 2).empty(), "one site: no fix");
    }
    {   // too far apart in time, and stale
        CrossFixer cf;
        cf.add(bearing("", kT0, 433.92e6, a_la, a_lo, ba));
        cf.add(bearing("b", kT0 + 8, 433.92e6, b_la, b_lo, bb));
        CHECK(cf.fixes(kT0 + 9).empty(), "8 s apart: no fix");
        CrossFixer old;
        old.add(bearing("", kT0, 433.92e6, a_la, a_lo, ba));
        old.add(bearing("b", kT0 + 1, 433.92e6, b_la, b_lo, bb));
        CHECK(old.fixes(kT0 + 60).empty(), "older than 30 s dropped");
    }
    {   // the newest bearing per site wins
        CrossFixer cf;
        cf.add(bearing("", kT0, 433.92e6, a_la, a_lo, ba + 40));
        cf.add(bearing("", kT0 + 2, 433.92e6, a_la, a_lo, ba));
        cf.add(bearing("b", kT0 + 3, 433.92e6, b_la, b_lo, bb));
        auto fx = cf.fixes(kT0 + 4);
        double d = 1e9, az;
        if (fx.size() == 1) geodInverse(fx[0].fix.lat, fx[0].fix.lon, tla, tlo, d, az);
        CHECK(fx.size() == 1 && d < 50, "newest per site: %zu %.1f", fx.size(), d);
    }
    {   // events without a position are ignored
        CrossFixer cf;
        au::Event e("df.bearing");
        e.at(kT0, "DF", 433.92e6);
        e.num("bearing_deg", 10);
        cf.add(e);
        CHECK(cf.fixes(kT0).empty(), "no position, no fix");
    }
    return testSummary("xfix");
}
