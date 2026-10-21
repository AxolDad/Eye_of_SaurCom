// Propagation model: known values of the public formulas, and the behaviours the HF estimate must have (day above night, loss falling with
// frequency, a skip zone, no skywave above the MUF).
#include "dect2/prop/prop.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2::prop;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (std::fabs(_a - _b) > (tol)) { printf("FAIL line %d: %s = %.4f, want %.4f\n", __LINE__, #a, _a, _b); fails++; } } while (0)

int main() {
    // P.525: 32.44 + 20 log f + 20 log d
    NEAR(fsplDb(100, 10), 92.44, 0.01);
    NEAR(fsplDb(1000, 1), 92.44, 0.01);
    NEAR(fsplDb(2400, 1), 100.04, 0.02);
    NEAR(fsplDb(100, 20) - fsplDb(100, 10), 6.02, 0.01);   // doubling the distance: 6 dB
    // thermal noise
    NEAR(ktbDbm(1), -174.0, 1e-9);
    NEAR(ktbDbm(1e6), -114.0, 1e-9);
    NEAR(ktbDbm(12500), -174 + 40.97, 0.01);
    // P.372 simplified: city 100 MHz = 76.8 - 27.7 * 2
    NEAR(externalNoiseFa(100, Env::City), 21.4, 0.01);
    NEAR(externalNoiseFa(10, Env::Residential), 44.8, 0.01);
    CHECK(externalNoiseFa(1000, Env::Quiet) < 0.5);                       // negligible at 1 GHz
    CHECK(externalNoiseFa(7, Env::City) > externalNoiseFa(7, Env::Rural));
    // system noise: external noise below the receiver's own leaves the receiver noise figure
    NEAR(systemNoiseDbm(1500, 1e6, 6, Env::Quiet), -114 + 6, 0.05);
    CHECK(systemNoiseDbm(7, 2400, 6, Env::City) > systemNoiseDbm(7, 2400, 6, Env::Quiet) + 20);
    // field strength: -100 dBm at 100 MHz, 0 dBi: E = -100 + 40 + 77.2
    NEAR(fieldDbuvm(-100, 100, 0), 17.2, 1e-9);
    NEAR(fieldDbuvm(-100, 100, 3), 14.2, 1e-9);
    // radio horizon 4/3 earth: 4.12 (sqrt h1 + sqrt h2)
    NEAR(radioHorizonKm(10, 2), 4.12 * (std::sqrt(10.0) + std::sqrt(2.0)), 1e-9);
    NEAR(radioHorizonKm(0, 0), 0, 1e-12);
    // diffraction: none inside the horizon, growing beyond it
    CHECK(diffractionLossDb(150, 10, 30, 2) == 0);
    {
        const double h = radioHorizonKm(30, 2);
        const double a = diffractionLossDb(150, h + 20, 30, 2), b = diffractionLossDb(150, h + 60, 30, 2), c = diffractionLossDb(150, h + 120, 30, 2);
        CHECK(a >= 0 && b > a && c > b);
        CHECK(diffractionLossDb(450, h + 60, 30, 2) > diffractionLossDb(150, h + 60, 30, 2));   // UHF is hurt more than VHF
        CHECK(diffractionLossDb(150, h + 60, 100, 2) < b);                                       // a taller mast reaches farther
    }
    // great circle and destination agree
    NEAR(greatCircleKm(0, 0, 0, 1), 111.19, 0.05);
    {
        double la, lo; destination(54.7, 25.3, 90, 500, la, lo);
        NEAR(greatCircleKm(54.7, 25.3, la, lo), 500, 0.01);
        double ml, mo; midpoint(0, 0, 0, 90, ml, mo);
        NEAR(ml, 0, 1e-9); NEAR(mo, 45, 1e-9);
    }
    // sunspot from flux
    NEAR(sfiToSunspot(63.7), 0, 1e-9);
    NEAR(sfiToSunspot(63.7 + 0.728 * 100 + 0.00089 * 1e4), 100, 0.01);
    // sun: equinox noon on the equator at lon 0 is overhead; midnight is not
    NEAR(solarCosZenith(0, 0, 2026, 3, 21, 12.0), 1.0, 0.02);
    CHECK(solarCosZenith(0, 0, 2026, 3, 21, 0.0) < -0.95);
    CHECK(solarCosZenith(60, 0, 2026, 6, 21, 12.0) > solarCosZenith(60, 0, 2026, 12, 21, 12.0));   // summer sun higher
    // hop geometry: vertical at zero distance, more slanted farther
    NEAR(hopIncidenceSec(0, 300), 1.0, 1e-9);
    CHECK(hopIncidenceSec(2000, 300) > hopIncidenceSec(500, 300));
    CHECK(hopIncidenceSec(4000, 300) > 2.5 && hopIncidenceSec(4000, 300) < 4.5);

    // VHF link: 10 W (10 dBW) EIRP at 100 MHz, 10 km, 12.5 kHz, quiet receiver
    Params v; v.fMHz = 100; v.eirpDbw = 10; v.txHeightM = 30; v.rxHeightM = 2; v.env = Env::Quiet; v.rxNfDb = 6; v.bandwidthHz = 12500; v.requiredSnrDb = 12;
    const Point a = evaluate(v, 10, 0);
    NEAR(a.pathLossDb, 92.44, 0.01);
    NEAR(a.rxDbm, 40 - 92.44, 0.01);
    CHECK(a.model == Model::LineOfSight);
    NEAR(a.snrDb, a.rxDbm - a.noiseDbm, 1e-9);
    NEAR(a.marginDb, a.snrDb - 12, 1e-9);
    NEAR(a.fieldDbuvm, a.rxDbm + 40 + 77.2, 1e-9);
    const Point far = evaluate(v, 150, 0);
    CHECK(far.model == Model::Diffraction && far.pathLossDb > fsplDb(100, 150));
    {   // range: margin >= 0 up to maxKm, below zero beyond it, and monotonic in power
        const Range r = findRange(v);
        CHECK(r.any && r.maxKm > 10);
        CHECK(evaluate(v, r.maxKm * 1.1, 0).marginDb < 0);
        Params v2 = v; v2.eirpDbw += 20;
        CHECK(findRange(v2).maxKm > r.maxKm);
        Params v3 = v; v3.requiredSnrDb += 20;
        CHECK(findRange(v3).maxKm < r.maxKm);
    }

    // HF: 7.1 MHz, midlatitude, 100 W (20 dBW), SFI 120, 1000 km
    Params h; h.txLat = 54.7; h.txLon = 25.3; h.fMHz = 7.1; h.eirpDbw = 20; h.env = Env::Quiet; h.bandwidthHz = 2400; h.sfi = 120; h.year = 2026; h.month = 6; h.day = 21;
    h.requiredSnrDb = 10; h.utcHour = 10.0;
    const double fo = foF2(h.txLat, h.txLon, h);
    CHECK(fo > 4 && fo < 14);
    Params night = h; night.utcHour = 22.0;
    CHECK(foF2(night.txLat, night.txLon, night) < fo);
    Params hi = h; hi.sfi = 200;
    CHECK(foF2(hi.txLat, hi.txLon, hi) > fo);
    CHECK(mufMHz(54.7, 25.3, 2000, h) > fo);                                        // oblique MUF above the vertical critical frequency
    CHECK(mufMHz(54.7, 25.3, 3000, h) > mufMHz(54.7, 25.3, 1000, h));
    {
        // skywave absorption: heavier by day than by night, lighter on higher frequencies
        Params hd = h, hn = night;
        hd.fMHz = hn.fMHz = 3.6;   // under the night MUF too, so both have a skywave
        const Point d1 = evaluate(hd, 1000, 90), n1 = evaluate(hn, 1000, 90);
        CHECK(d1.hops == 1 && n1.hops == 1);
        CHECK(d1.absorptionDb > n1.absorptionDb);
        CHECK(evaluate(h, 1000, 90).absorptionDb < d1.absorptionDb);   // 7.1 MHz is absorbed less than 3.6 MHz
        // far away the skywave carries it; the ground wave does not
        const Point s = evaluate(h, 1500, 90);
        CHECK(s.model == Model::HfSky);
        // two hops for 6000 km
        CHECK(evaluate(h, 6000, 90).hops == 2);
        // ground wave dominates close in
        CHECK(evaluate(h, 3, 90).model == Model::HfGround);
        // no skywave above the MUF: 28 MHz at night
        Params t = night; t.fMHz = 28.0;
        CHECK(evaluate(t, 1500, 90).hops == 0);
        // sea carries the ground wave better than dry ground
        CHECK(groundWaveFactor(7.1, 100, Ground::Sea) > groundWaveFactor(7.1, 100, Ground::Dry));
        NEAR(groundWaveFactor(7.1, 0.0, Ground::Average), 1.0, 1e-9);
        CHECK(groundWaveFactor(7.1, 200, Ground::Average) < groundWaveFactor(7.1, 20, Ground::Average));
    }
    // grid: the transmitter cell is the strongest; same values as evaluateAt
    {
        Grid g = computeGrid(v, 50, 21);
        CHECK(g.n == 21 && g.snr.size() == 441);
        CHECK(g.snr[10 * 21 + 10] > g.snr[0]);
        const double la = g.lat0 + (g.lat1 - g.lat0) * 10.5 / 21, lo = g.lon0 + (g.lon1 - g.lon0) * 10.5 / 21;
        NEAR(g.snr[10 * 21 + 10], evaluateAt(v, la, lo).snrDb, 1e-4);
    }
    CHECK(std::string(modelName(h)) != std::string(modelName(v)));
    CHECK(!modelLimits(h).empty() && !modelLimits(v).empty());
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("prop_model: ok\n");
    return 0;
}
