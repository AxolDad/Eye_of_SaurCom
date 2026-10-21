// Propagation prediction, see core/include/dect2/prop/prop.h and docs/propagation.md.
#include "dect2/prop/prop.h"
#include <algorithm>
#include <cmath>

namespace dect2::prop {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRe = 6371.0;           // km
constexpr double kAe = 8495.0;           // km, 4/3 earth radius
constexpr double kDeg = kPi / 180.0;
constexpr double kHfMax = 30.0;          // MHz: below this the HF model, from this up the line-of-sight model
constexpr double kHopMaxKm = 4000.0;     // longest single F2 hop
constexpr double kF2HeightKm = 300.0, kDHeightKm = 100.0;

double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
double groundSigma(Ground g) {   // S/m
    switch (g) { case Ground::Sea: return 5.0; case Ground::Wet: return 0.01; case Ground::Dry: return 0.001; default: return 0.005; }
}
double hopSlantKm(double hopKm, double hKm) {   // path length of one hop up to a layer and down
    const double th = hopKm / (2 * kRe);
    return 2 * std::sqrt(kRe * kRe + (kRe + hKm) * (kRe + hKm) - 2 * kRe * (kRe + hKm) * std::cos(th));
}
}

double fsplDb(double fMHz, double dKm) { return 32.44 + 20 * std::log10(fMHz) + 20 * std::log10(std::max(dKm, 1e-6)); }
double ktbDbm(double bandwidthHz) { return -174.0 + 10 * std::log10(std::max(bandwidthHz, 1.0)); }

double externalNoiseFa(double fMHz, Env env) {
    double c = 72.5, d = 27.7;
    switch (env) {
        case Env::City: c = 76.8; d = 27.7; break;
        case Env::Residential: c = 72.5; d = 27.7; break;
        case Env::Rural: c = 67.2; d = 27.7; break;
        case Env::Quiet: c = 53.6; d = 28.6; break;
    }
    const double lf = std::log10(std::max(fMHz, 0.01));
    const double man = c - d * lf, gal = 52.0 - 23.0 * lf;
    return std::max(0.0, std::max(man, gal));
}

double systemNoiseDbm(double fMHz, double bandwidthHz, double nfDb, Env env) {
    const double fa = std::pow(10.0, externalNoiseFa(fMHz, env) / 10.0), fr = std::pow(10.0, std::max(0.0, nfDb) / 10.0);
    return ktbDbm(bandwidthHz) + 10 * std::log10(fa + fr - 1.0);
}

double fieldDbuvm(double rxDbm, double fMHz, double rxGainDbi) { return rxDbm - rxGainDbi + 20 * std::log10(fMHz) + 77.2; }

double radioHorizonKm(double h1M, double h2M) { return 4.12 * (std::sqrt(std::max(h1M, 0.0)) + std::sqrt(std::max(h2M, 0.0))); }

double diffractionLossDb(double fMHz, double dKm, double h1M, double h2M) {
    if (dKm <= radioHorizonKm(h1M, h2M)) return 0;
    const double X = 2.2 * std::cbrt(fMHz) * std::pow(kAe, -2.0 / 3.0) * dKm;
    const double F = X >= 1.6 ? 11 + 10 * std::log10(X) - 17.6 * X : -20 * std::log10(X) - 5.6488 * std::pow(X, 1.425);
    auto G = [&](double hM) {
        const double Y = 9.6e-3 * std::pow(fMHz, 2.0 / 3.0) * std::pow(kAe, -1.0 / 3.0) * hM;
        const double B = Y;   // beta = 1 for horizontal / vertical polarisation above 20 MHz over land (simplification)
        return B > 2 ? 17.6 * std::sqrt(B - 1.1) - 5 * std::log10(B - 1.1) - 8 : 20 * std::log10(std::max(B + 0.1 * B * B * B, 1e-6));
    };
    return std::max(0.0, -(F + G(h1M) + G(h2M)));
}

double groundWaveFactor(double fMHz, double dKm, Ground g) {
    const double lambda = 300.0 / fMHz, x = 1.8e4 * groundSigma(g) / fMHz;
    const double p = kPi * dKm * 1000.0 / (lambda * x);
    return (2 + 0.3 * p) / (2 + p + 0.6 * p * p);
}

double greatCircleKm(double lat1, double lon1, double lat2, double lon2) {
    const double a1 = lat1 * kDeg, a2 = lat2 * kDeg, dl = (lon2 - lon1) * kDeg, dp = a2 - a1;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(a1) * std::cos(a2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kRe * std::asin(std::min(1.0, std::sqrt(h)));
}

void destination(double lat, double lon, double bearingDeg, double distKm, double& lat2, double& lon2) {
    const double d = distKm / kRe, b = bearingDeg * kDeg, p1 = lat * kDeg, l1 = lon * kDeg;
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    lat2 = p2 / kDeg;
    lon2 = std::fmod(l2 / kDeg + 540.0, 360.0) - 180.0;
}

void midpoint(double lat1, double lon1, double lat2, double lon2, double& lat, double& lon) {
    const double p1 = lat1 * kDeg, p2 = lat2 * kDeg, l1 = lon1 * kDeg, dl = (lon2 - lon1) * kDeg;
    const double bx = std::cos(p2) * std::cos(dl), by = std::cos(p2) * std::sin(dl);
    lat = std::atan2(std::sin(p1) + std::sin(p2), std::sqrt((std::cos(p1) + bx) * (std::cos(p1) + bx) + by * by)) / kDeg;
    lon = std::fmod((l1 + std::atan2(by, std::cos(p1) + bx)) / kDeg + 540.0, 360.0) - 180.0;
}

double sfiToSunspot(double sfi) {   // inverts SFI = 63.7 + 0.728 R + 0.00089 R^2
    const double s = std::max(sfi, 63.7);
    return (-0.728 + std::sqrt(0.728 * 0.728 + 4 * 0.00089 * (s - 63.7))) / (2 * 0.00089);
}

double solarCosZenith(double lat, double lon, int y, int m, int d, double utcHour) {
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    const int doy = cum[(int)clampd(m, 1, 12) - 1] + d + (leap && m > 2 ? 1 : 0);
    const double decl = 23.44 * kDeg * std::sin(2 * kPi * (doy - 81) / 365.0);
    const double H = (15.0 * (utcHour - 12.0) + lon) * kDeg;
    return std::sin(lat * kDeg) * std::sin(decl) + std::cos(lat * kDeg) * std::cos(decl) * std::cos(H);
}

double foF2(double lat, double lon, const Params& p) {
    const double R = sfiToSunspot(p.sfi);
    const double latF = 1.0 - 0.35 * std::pow(std::sin(lat * kDeg), 2);
    const double day = (5.5 + 0.045 * R) * latF, night = 0.45 * day;
    const double c = clampd((solarCosZenith(lat, lon, p.year, p.month, p.day, p.utcHour) + 0.1) / 1.1, 0.0, 1.0);
    return night + (day - night) * std::sqrt(c);
}

double hopIncidenceSec(double hopKm, double layerKm) {
    const double th = hopKm / (2 * kRe);
    const double t = kRe * std::sin(th) / ((kRe + layerKm) - kRe * std::cos(th));   // tan(incidence angle at the layer)
    return std::sqrt(1 + t * t);
}

double mufMHz(double lat, double lon, double distKm, const Params& p) {
    const double hop = distKm / std::max(1.0, std::ceil(distKm / kHopMaxKm));
    return foF2(lat, lon, p) * hopIncidenceSec(hop, kF2HeightKm);
}

static Point evalInternal(const Params& p, double distKm, double midLat, double midLon) {
    Point o;
    o.distKm = distKm;
    const double d = std::max(distKm, 0.05);
    o.noiseDbm = systemNoiseDbm(p.fMHz, p.bandwidthHz, p.rxNfDb, p.env);
    o.horizonKm = radioHorizonKm(p.txHeightM, p.rxHeightM);
    double pl = 0;
    if (p.fMHz >= kHfMax) {
        const double dl = diffractionLossDb(p.fMHz, d, p.txHeightM, p.rxHeightM);
        pl = fsplDb(p.fMHz, d) + dl;
        o.model = dl > 0 ? Model::Diffraction : Model::LineOfSight;
    } else {
        // ground wave
        const double A = std::max(groundWaveFactor(p.fMHz, d, p.ground), 1e-9);
        const double gw = fsplDb(p.fMHz, d) - 20 * std::log10(A);
        // skywave: the fewest hops of at most 4000 km; it exists only when the frequency is under the MUF of that hop
        double sw = 1e9;
        const int n = (int)std::max(1.0, std::ceil(d / kHopMaxKm));
        const double hop = d / n;
        o.muf = mufMHz(midLat, midLon, d, p);
        if (p.fMHz <= o.muf) {
            const double R = sfiToSunspot(p.sfi);
            const double chi = std::acos(clampd(solarCosZenith(midLat, midLon, p.year, p.month, p.day, p.utcHour), -1, 1));
            const double cosTerm = std::pow(std::max(std::cos(0.881 * std::min(chi, kPi / 2)), 0.0), 1.3);
            const double Ir = (1 + 0.0037 * R) * std::max(cosTerm, 0.1);
            const double La = 677.2 * Ir * hopIncidenceSec(hop, kDHeightKm) / (std::pow(p.fMHz + 1.0, 1.98) + 10.2);
            o.hops = n;
            o.absorptionDb = n * La;
            sw = fsplDb(p.fMHz, n * hopSlantKm(hop, kF2HeightKm)) + o.absorptionDb + 2.0 * (n - 1);
        }
        // power sum of the two
        pl = -10 * std::log10(std::pow(10.0, -gw / 10) + std::pow(10.0, -sw / 10));
        o.model = sw < gw ? Model::HfSky : Model::HfGround;
    }
    o.pathLossDb = pl;
    o.rxDbm = p.eirpDbw + 30.0 - pl + p.rxGainDbi;
    o.fieldDbuvm = fieldDbuvm(o.rxDbm, p.fMHz, p.rxGainDbi);
    o.snrDb = o.rxDbm - o.noiseDbm;
    o.marginDb = o.snrDb - p.requiredSnrDb;
    return o;
}

Point evaluate(const Params& p, double distKm, double bearingDeg) {
    double la, lo, ml, mo;
    destination(p.txLat, p.txLon, bearingDeg, distKm, la, lo);
    midpoint(p.txLat, p.txLon, la, lo, ml, mo);
    return evalInternal(p, distKm, ml, mo);
}

Point evaluateAt(const Params& p, double lat, double lon) {
    double ml, mo;
    midpoint(p.txLat, p.txLon, lat, lon, ml, mo);
    return evalInternal(p, greatCircleKm(p.txLat, p.txLon, lat, lon), ml, mo);
}

Range findRange(const Params& p, double bearingDeg) {
    Range r;
    const int steps = 600;
    const double d0 = 0.05, d1 = 20000.0;
    for (int i = 0; i <= steps; i++) {
        const double d = d0 * std::pow(d1 / d0, (double)i / steps);
        if (evaluate(p, d, bearingDeg).marginDb >= 0) {
            if (!r.any) { r.any = true; r.minKm = d <= d0 * 1.05 ? 0 : d; }
            r.maxKm = d;
        }
    }
    return r;
}

Grid computeGrid(const Params& p, double radiusKm, int n) {
    Grid g;
    n = std::max(2, n);
    g.n = n;
    g.rangeKm = radiusKm;
    const double dLat = radiusKm / 111.19;
    const double dLon = dLat / std::max(0.05, std::cos(clampd(p.txLat, -85, 85) * kDeg));
    g.lat0 = clampd(p.txLat - dLat, -85, 85); g.lat1 = clampd(p.txLat + dLat, -85, 85);
    g.lon0 = p.txLon - dLon; g.lon1 = p.txLon + dLon;
    g.snr.resize((size_t)n * n);
    g.model.resize((size_t)n * n);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const double la = g.lat0 + (g.lat1 - g.lat0) * (y + 0.5) / n, lo = g.lon0 + (g.lon1 - g.lon0) * (x + 0.5) / n;
            const Point q = evaluateAt(p, la, lo);
            g.snr[(size_t)y * n + x] = (float)q.snrDb;
            g.model[(size_t)y * n + x] = (unsigned char)q.model;
        }
    return g;
}

const char* modelName(const Params& p) {
    return p.fMHz < kHfMax ? "HF: ground wave + skywave estimate" : "VHF/UHF: line of sight + smooth-earth diffraction";
}

std::string modelLimits(const Params& p) {
    if (p.fMHz < kHfMax)
        return "HF below 30 MHz: Norton ground wave plus a one-layer F2 skywave (foF2 from time of day, season, latitude and the SFI you enter, "
               "D-layer absorption). An estimate within a few MHz and 10 dB or more. No E layer, sporadic E, tilts, storms, auroral or equatorial effects, "
               "no ground-wave land/sea mixing, no antenna patterns, no terrain.";
    if (p.fMHz > 6000.0)
        return "Above 6 GHz rain and gas absorption matter and are not modelled; treat the range as an upper bound. No terrain, buildings or foliage.";
    return "30 MHz and up: free space inside the 4/3-earth radio horizon, smooth-earth diffraction beyond it. No terrain, buildings, foliage, "
           "ducting, tropospheric scatter, rain or sporadic E; real coverage is usually shorter in cities and hills.";
}

const char* pointModelName(Model m) {
    switch (m) {
        case Model::FreeSpace: return "free space";
        case Model::LineOfSight: return "line of sight (free space)";
        case Model::Diffraction: return "smooth-earth diffraction";
        case Model::HfGround: return "HF ground wave";
        case Model::HfSky: return "HF skywave";
    }
    return "";
}

} // namespace dect2::prop
