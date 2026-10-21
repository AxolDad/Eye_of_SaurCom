// Propagation prediction (pure functions, no terrain, no downloaded data). Clean-room from public formulas, see docs/propagation.md:
//  - free space: ITU-R P.525; noise: kTB plus man-made / galactic noise, ITU-R P.372 simplified table
//  - 30 MHz and up: line of sight with a 4/3 earth radio horizon, smooth-earth diffraction beyond it (ITU-R P.526 simplified)
//  - below 30 MHz: ground wave (Norton, Terman's form) plus a one-layer skywave estimate (foF2 from time, season, latitude and SFI;
//    D-layer absorption after ITU-R P.533), an ESTIMATE only. Sporadic E and anything terrain-related are not predicted.
#pragma once
#include <string>
#include <vector>

namespace dect2::prop {

enum class Env { Quiet = 0, Rural, Residential, City };   // man-made noise environment at the receiver
enum class Ground { Sea = 0, Wet, Average, Dry };         // ground conductivity for the HF ground wave
enum class Model { FreeSpace = 0, LineOfSight, Diffraction, HfGround, HfSky };   // which part decided the loss at a point

struct Params {
    double txLat = 0, txLon = 0;   // transmitter position, degrees
    double eirpDbw = 0;            // radiated power toward the receiver, dBW (power + antenna gain - feeder loss)
    double fMHz = 100;             // carrier frequency
    double bandwidthHz = 12500;    // noise bandwidth of the receiver
    double rxGainDbi = 0;          // receive antenna gain
    double rxNfDb = 6;             // receiver noise figure
    Env env = Env::Residential;
    double txHeightM = 10, rxHeightM = 2;
    Ground ground = Ground::Average;
    double sfi = 120;              // 10.7 cm solar flux, sfu (HF only)
    int year = 2026, month = 1, day = 1;
    double utcHour = 12;           // 0..24
    double requiredSnrDb = 10;
};

struct Point {
    double distKm = 0, pathLossDb = 0, rxDbm = 0, fieldDbuvm = 0, noiseDbm = 0, snrDb = 0, marginDb = 0;
    Model model = Model::FreeSpace;
    double horizonKm = 0;          // radio horizon of the pair (VHF and up)
    double muf = 0;                // HF: MUF of the path, MHz (0 when not HF)
    double absorptionDb = 0;       // HF: D-layer absorption of the skywave
    int hops = 0;                  // HF: skywave hops (0: no skywave at this distance and frequency)
};

// Building blocks (documented in docs/propagation.md), exposed for tests
double fsplDb(double fMHz, double dKm);                          // P.525: 32.44 + 20 log f + 20 log d
double ktbDbm(double bandwidthHz);                               // -174 dBm/Hz + 10 log B
double externalNoiseFa(double fMHz, Env env);                    // P.372: dB above kTb, the larger of man-made and galactic
double systemNoiseDbm(double fMHz, double bandwidthHz, double nfDb, Env env);   // kTB raised by the external noise and the receiver noise
double fieldDbuvm(double rxDbm, double fMHz, double rxGainDbi);  // E at the receiver from the received power
double radioHorizonKm(double h1M, double h2M);                   // 4/3 earth
double diffractionLossDb(double fMHz, double dKm, double h1M, double h2M);   // P.526 smooth earth, 0 inside the horizon
double groundWaveFactor(double fMHz, double dKm, Ground g);      // Norton attenuation A (0..1)
double solarCosZenith(double lat, double lon, int y, int m, int d, double utcHour);
double foF2(double lat, double lon, const Params& p);            // estimate, MHz
double hopIncidenceSec(double hopKm, double layerKm);            // sec of the incidence angle at a layer of that height
double mufMHz(double lat, double lon, double distKm, const Params& p);
double greatCircleKm(double lat1, double lon1, double lat2, double lon2);
void destination(double lat, double lon, double bearingDeg, double distKm, double& lat2, double& lon2);
void midpoint(double lat1, double lon1, double lat2, double lon2, double& lat, double& lon);
double sfiToSunspot(double sfi);

// One point at a ground distance from the transmitter, along a bearing (the bearing only places the HF midpoint)
Point evaluate(const Params& p, double distKm, double bearingDeg);
// The path to a latitude / longitude
Point evaluateAt(const Params& p, double lat, double lon);

struct Range {
    double maxKm = 0;       // farthest distance with the margin >= 0 (0: none)
    double minKm = 0;       // nearest such distance (a skip zone below it for HF; ~0 when the signal is also heard near the transmitter)
    bool any = false;
};
Range findRange(const Params& p, double bearingDeg = 0);

struct Grid {
    int n = 0;                         // n x n cells
    double lat0 = 0, lon0 = 0, lat1 = 0, lon1 = 0;   // south-west and north-east corners
    double rangeKm = 0;                // half width of the square
    std::vector<float> snr;            // row 0 = south
    std::vector<unsigned char> model;
};
// Square of +-radiusKm around the transmitter, n x n cells (cheap enough to run on a worker thread)
Grid computeGrid(const Params& p, double radiusKm, int n);

const char* modelName(const Params& p);          // the model chosen by frequency
std::string modelLimits(const Params& p);        // what it covers and what it does not
const char* pointModelName(Model m);

} // namespace dect2::prop
